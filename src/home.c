/** @file Home's jobs: admission, the scheduler, the runs of the jobs over
 *  the curl bridge, streamed answers and the breaker (home.h). */
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
	CANCEL_MS    = 5'000,     /* native cancellation's total lifetime */
	MAX_JOB_ID   = 4096,      /* bytes of a native provider job's id */
	KEYS_MAX     = 1 << 16,   /* the largest number of buckets of the
	                             principals */
	SLOTS        = 4,         /* upstream calls at once, unless told or found */
	FAILURES     = 5,         /* failures in a row that open the breaker */
};

static constexpr uint64_t NS = 1'000'000'000;
static constexpr uint64_t BACKOFF_MIN = NS;
static constexpr uint64_t BACKOFF_MAX = 60 * NS;

/* What home notes of a job's run, in its flags. */
enum {
	OVERFLOW  = 1 << 0,   /* the answer passed MAX_RESPONSE */
	DROPPED   = 1 << 1,   /* its front let go of it while it ran */
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
struct home_principal {
	struct list            ring;    /* on home's ring of its class, or its shelf */
	struct list            jobs;    /* queued, oldest first, by their queue hooks */
	struct home_principal *chain;   /* the next in its bucket */
	uint64_t               hash;
	uint32_t               queued;
	uint32_t               class;   /* 0 interactive, 1 background */
};

/* Best-effort cancellation outlives the local job, but not its deadline. */
struct home_cancellation {
	struct list          hook;
	struct http_request *transfer;
	uint64_t             deadline;
	size_t               received;
};

/* Only failed native streams need this: buffered events can still carry
 * the id that lets us cancel the provider's generation. */
struct home_delivery_error {
	long     retry;
	CURLcode result;
	char     message[CURL_ERROR_SIZE];
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
 * libcurl costs some tens of µs.  A seeded hash of a principal selects the
 * bucket of the principal, and each bucket is a chained list.
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
	struct loop_step          cleanup;     /* ends overdue native cancellations */
	struct loop_timer         deadline;    /* the timeline's first */
	struct loop_timer         quiet;       /* the answer silent longest */
	struct loop_timer         breaker;     /* when the breaker half-opens */
	struct loop_timer         cancel_due;  /* the first cancellation's deadline */
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
	struct list               cancellations; /* native cancellations, oldest first */
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
	struct home_principal   **whom;        /* the principals' buckets */
	struct http_request      *ask;         /* GET /props, under way */
	uint64_t                  timeout;     /* ns from a start to its deadline */
	uint64_t                  silence;     /* ns an answer may pause */
	uint64_t                  backoff;     /* the breaker's next open spell */
	uint64_t                  until;       /* when the breaker half-opens */
	size_t                    mask;        /* buckets - 1 */
	uint32_t                  max;         /* jobs in flight at most */
	uint32_t                  slots;       /* upstream calls at once at most */
	uint32_t                  context;     /* tokens of the upstream's context,
	                                          as told or found; 0: unknown */
	uint32_t                  failures;    /* in a row, closed */
	uint32_t                  readied[2];  /* jobs ready, by class */
	uint32_t                  running_bg;  /* background jobs running */
	uint32_t                  cancelling;  /* native cancellation requests */
	uint32_t                  draining;    /* bool, widened over the padding */
	uint32_t                  alone;       /* bool, widened over the padding:
	                                          the client loops have exited */
	uint8_t                   state;       /* the breaker's */
	uint8_t                   probing;     /* bool: a probe is under way */
	uint8_t                   paused;      /* bool: /props is held back */
	uint8_t                   fifo;        /* bool: one queue for all */
};

/* The thread's home: the handler of jobs' messages finds it here. */
static thread_local struct home *self;

/* @p job's class: 0 interactive, 1 background. */
static uint32_t
home_job_class (struct job const *job)
{
	return (job->asks & JOB_BACKGROUND) != 0;
}

/* The upstream slots that background jobs may hold at most: all but one,
 * which stays for interactive work, unless there is only one. */
static uint32_t
home_background_slots (struct home const *home)
{
	return home->slots - (home->slots >= 2);
}

/* Whether @p job is for @p p, who its first job queued is for. */
static bool
home_principal_is (struct home_principal const *p,
                   struct job const            *job)
{
	struct job const *first = container_of(p->jobs.next, struct job, queue);
	return p->hash == job->principal && p->class == home_job_class(job) &&
	       first->session == job->session &&
	       (job->session ? !memcmp(buf_peek(&first->key).ptr,
	                               buf_peek(&job->key).ptr, job->session)
	                     : first->peer[0] == job->peer[0] &&
	                       first->peer[1] == job->peer[1]);
}

/* The link that holds the principal @p job is for, or where it would be
 * appended; for all of a class one in a queue first in first out. */
static struct home_principal **
home_find_whom (struct home const *home,
                struct job const  *job)
{
	struct home_principal **p = home->whom;
	if (home->fifo) {
		uint32_t c = home_job_class(job);
		while (*p && (*p)->class != c)
			p = &(*p)->chain;
		return p;
	}
	p += job->principal & home->mask;
	while (*p && !home_principal_is(*p, job))
		p = &(*p)->chain;
	return p;
}

/* Publish @p job's progress, and tell its front if it asked. */
static void
home_job_progress (struct job *job,
                   uint64_t    progress)
{
	atomic_store_explicit(&job->progress, progress, memory_order_relaxed);
	if (job->asks & JOB_NOTIFY && !(job->flags & DROPPED))
		job_post(job, job->front, &job->to_front, &job->front_bits, JOB_PROGRESS);
}

/* Take @p job off its principal's queue, the jobs behind it each a place
 * nearer, at most JOB_QUEUED - 1 (in one queue for all, which has no cap,
 * they keep the places they had); a principal with none left goes on the
 * shelf. */
static void
home_unqueue (struct home *home,
              struct job  *job)
{
	struct home_principal *p = job->who;
	struct list *node = job->queue.next;
	list_del(&job->queue);
	job->who = nullptr;
	hist_add(&home->metrics->queued, -1ULL);
	if (!--p->queued) {
		struct home_principal **link = home->whom;
		link += home->fifo ? 0 : p->hash & home->mask;
		while (*link != p)
			link = &(*link)->chain;
		*link = p->chain;
		list_del(&p->ring);
		shelf_put(&home->spare, &p->ring);
		hist_add(&home->metrics->principals, -1ULL);
		return;
	}
	if (home->fifo)
		return;
	for (; node != &p->jobs; node = node->next) {
		struct job *next = container_of(node, struct job, queue);
		home_job_progress(next, atomic_load_explicit(&next->progress,
		                                             memory_order_relaxed) - 1);
	}
}

/* Give the storage of @p job's answer back to the stash. */
static void
home_job_drop_answer (struct job *job)
{
	buf_fini(&job->result);
	for (size_t k = 0; k < JOB_CALLS; ++k)
		buf_fini(&job->calls[k]);
	buf_fini(&job->thought);
	buf_fini(&job->replay);
}

/* Drop @p n references to @p job, and hand it back to its front if they were
 * the last. */
static void
home_job_put (struct job *job,
              uint32_t    n)
{
	if (job_put(job, n)) {
		atomic_store_explicit(&job->front_bits, JOB_RETURN, memory_order_relaxed);
		loop_post(job->front, &job->to_front);
	}
}

/* Forget what home noted of @p job's last run, its flags now @p flags. */
static void
home_job_reset (struct job *job,
                uint16_t    flags)
{
	job->flags = flags;
	job->http = 0;
	job->ev = job->evlen = job->line = job->scan = job->chunks = job->used = 0;
	job->reader = (struct ai_reader){};
	job->provider_done = false;
	job->reason[0] = '\0';
}

/* @p job has ended as @p status: its completion goes to its front.  The
 * result of a job cancelled, which its front does not read, goes back to the
 * stash at once, and so does that of one its front let go of as it ran,
 * which home lets go of too. */
static void
home_job_end (struct job      *job,
              enum job_status  status)
{
	job->status = (uint8_t)status;
	if (job->flags & DROPPED) {
		home_job_drop_answer(job);
		job_post(job, job->front, &job->to_front, &job->front_bits, JOB_DONE);
		home_job_put(job, 1);
		return;
	}
	if (status == JOB_CANCELLED)
		home_job_drop_answer(job);
	job_post(job, job->front, &job->to_front, &job->front_bits, JOB_DONE);
}

/* Have the reap step free the events' values left, if any are. */
static void
home_reap (struct home *home)
{
	if (home->reaper.n)
		loop_ready_bulk(home->loop, &home->reap);
}

/* Let go of what @p job's answer under way holds: its places among the
 * answers silent, held back and to read, its event's parser, whose values
 * the reap step frees, and its wire. */
static void
home_unstream (struct home *home,
               struct job  *job)
{
	list_del(&job->idle);
	list_del(&job->feed);
	list_del(&job->held);
	jb_parser_fini(&job->parser, &home->pool, &home->reaper);
	home_reap(home);
	buf_fini(&job->wire);
	curl_free(job->cancel_url);
	job->cancel_url = nullptr;
	free(job->delivery_error);
	job->delivery_error = nullptr;
}

/* Takes @p job, which is in flight, off the timeline, off its principal's
 * queue and off its list of jobs.  If the job runs upstream, the function
 * frees its upstream slot, so that the next job can start. */
static void
home_retire (struct home *home,
             struct job  *job)
{
	struct home_metrics *m = home->metrics;
	uint32_t c = home_job_class(job);
	list_del(&job->due);
	hist_add(&m->in_flight, -1ULL);
	if (job->who)
		home_unqueue(home, job);
	home->readied[c] -= (job->flags & READY) != 0;
	list_del(&job->queue);
	if (job->flags & RUNNING) {
		hist_add(&m->running, -1ULL);
		if (c) {
			hist_add(&m->background, -1ULL);
			--home->running_bg;
		}
		home->probing &= !(job->flags & PROBE);
		loop_ready_bulk(home->loop, &home->start);
	}
	home_unstream(home, job);
}

/* On shutdown, leave time for the bounded native cancellations too. */
static void
home_maybe_exit (struct home *home)
{
	if (home->alone && !hist_get(&home->metrics->in_flight) && !home->cancelling)
		loop_exit(home->loop);
}

/* Forget a completed or expired native cancellation, and time the next. */
static void
home_cancel_drop (struct home              *home,
                  struct home_cancellation *cancel)
{
	list_del(&cancel->hook);
	free(cancel);
	cancel = nullptr;
	--home->cancelling;
	uint64_t due = list_is_empty(&home->cancellations) ? UINT64_MAX
	             : container_of(home->cancellations.next, struct home_cancellation,
	                            hook)->deadline;
	(void)loop_timer_set(home->loop, &home->cancel_due, due);
	home_maybe_exit(home);
}

/* The cancellation's answer is immaterial and bounded, even on an error. */
static size_t
home_cancel_write (char const *data,
                   size_t      len,
                   void       *user)
{
	(void)data;
	struct home_cancellation *cancel = user;
	if (len > (64u << 10) - cancel->received)
		return 0;
	cancel->received += len;
	loop_work(self->loop, LOOP_RECV, len);
	return len;
}

static void
home_cancel_done (CURLcode    result,
                  long        status,
                  char const *error,
                  void       *user)
{
	(void)error;
	struct home_cancellation *cancel = user;
	cancel->transfer = nullptr;
	if (result || status < 200 || status > 299)
		LOG(LOG_WARN, "native background cancellation unconfirmed (HTTP %ld, %s)",
		    status, curl_easy_strerror(result));
	home_cancel_drop(self, cancel);
}

/* Cancel native work independently of the local job's lifetime. Closing
 * its stream alone would leave it running at the provider. */
static void
home_native_cancel (struct home *home,
                    struct job  *job)
{
	if (!ai_background(home->ep) || !(job->flags & RUNNING) || job->provider_done)
		return;
	if (!job->cancel_url) {
		LOG(LOG_WARN, "native background cancellation unavailable: no provider job id");
		return;
	}
	if (home->cancelling >= home->max) {
		LOG(LOG_WARN, "native background cancellation unavailable: cleanup queue full");
		return;
	}
	struct home_cancellation *cancel = calloc(1, sizeof *cancel);
	if (!cancel) {
		LOG(LOG_WARN, "native background cancellation unavailable: out of memory");
		return;
	}
	cancel->deadline = home->loop->now + (uint64_t)CANCEL_MS * 1'000'000;
	int e = http_post_json(home->http, job->cancel_url, "", 0, false,
	                       home->ep->headers, CANCEL_MS, home_cancel_write,
	                       home_cancel_done, cancel, &cancel->transfer);
	if (e) {
		free(cancel);
		cancel = nullptr;
		LOG(LOG_WARN, "native background cancellation unavailable: %s", strerror(e));
		return;
	}
	list_append(&home->cancellations, &cancel->hook);
	++home->cancelling;
	if (home->cancel_due.due == UINT64_MAX)
		(void)loop_timer_set(home->loop, &home->cancel_due, cancel->deadline);
}

/* Bound cancellation even while the bridge waits for a resolver or slot. */
static void
home_cleanup_step (struct loop      *loop,
                   struct loop_step *step)
{
	struct home *home = container_of(step, struct home, cleanup);
	while (!list_is_empty(&home->cancellations)) {
		struct home_cancellation *cancel = container_of(home->cancellations.next,
		                                               struct home_cancellation, hook);
		if (cancel->deadline > loop->now)
			return;
		if (!loop_room(loop, LOOP_REQS)) {
			loop_ready_bulk(loop, step);
			return;
		}
		http_cancel(home->http, &cancel->transfer);
		home_cancel_done(CURLE_OPERATION_TIMEDOUT, 0, "", cancel);
		loop_work(loop, LOOP_REQS, 1);
	}
}

static void
home_cleanup_event (struct loop       *loop,
                    struct loop_timer *timer)
{
	loop_ready_bulk(loop, &container_of(timer, struct home, cancel_due)->cleanup);
}

/* End @p job as @p status, counted in @p count, leaving the jobs in flight
 * if it is one (home_job_end()).  Alone, home has its loop exit once no job
 * is in flight. */
static void
home_end (struct home      *home,
          struct job       *job,
          enum job_status   status,
          _Atomic uint64_t *count)
{
	if (status == JOB_CANCELLED || status == JOB_FAILED)
		home_native_cancel(home, job);
	if (!list_is_empty(&job->due))
		home_retire(home, job);
	hist_add(count, 1);
	home_job_end(job, status);
	home_maybe_exit(home);
}

/* Put what @p fmt says, formatted, in @p job's result in its answer's place;
 * returns 0 or the error number that left it empty. */
[[gnu::format(printf, 2, 3)]]
static int
home_job_say (struct job *job,
              char const *fmt,
              ...)
{
	buf_consume(&job->result, buf_peek(&job->result).len);
	for (size_t room = 128;;) {
		struct span s;
		int e = buf_reserve(&job->result, room, SIZE_MAX, &s);
		if (e)
			return e;
		va_list ap;
		va_start(ap, fmt);
		int n = vsnprintf((char *)s.ptr, s.len, fmt, ap);
		va_end(ap);
		if (n < 0)
			return EOVERFLOW;
		if ((size_t)n < s.len) {
			buf_commit(&job->result, (size_t)n);
			return 0;
		}
		room = (size_t)n + 1;
	}
}

/* The upstream erred, as the @p len bytes at @p what say, all of them if
 * @p len is negative: say so in @p job's result (home_job_say()). */
static int
home_job_erred (struct job *job,
                char const *what,
                int         len)
{
	job->flags |= ERRED;
	return home_job_say(job, "Upstream error (HTTP %d): %.*s", job->http, len, what);
}

/* The breaker is open: end @p job, which may not go upstream, with when to
 * try again. */
static void
home_refuse (struct home *home,
             struct job  *job)
{
	uint64_t now = home->loop->now;
	uint64_t left = home->until > now ? home->until - now : 0;
	(void)home_job_say(job, "The upstream is unavailable; try again in %" PRIu64 " s.",
	                   (left + NS - 1) / NS);
	home_end(home, job, JOB_FAILED, &home->metrics->rejected);
}

static void
home_breaker_set (struct home *home,
                  uint8_t      state)
{
	home->state = state;
	atomic_store_explicit(&home->metrics->breaker, state, memory_order_relaxed);
}

/* Refuse the upstream jobs for a spell: what Retry-After asked for, @p retry
 * seconds, or else the backoff, which doubles for the next. */
static void
home_breaker_open (struct home *home,
                   long         retry)
{
	struct loop *loop = home->loop;
	uint64_t spell = retry > 0 ? (uint64_t)retry * NS : home->backoff;
	spell = spell < BACKOFF_MIN ? BACKOFF_MIN : spell > BACKOFF_MAX ? BACKOFF_MAX : spell;
	home->backoff = home->backoff < BACKOFF_MAX / 2 ? 2 * home->backoff : BACKOFF_MAX;
	home->until = loop->now + spell;
	home_breaker_set(home, OPEN);
	hist_add(&home->metrics->opens, 1);
	(void)loop_timer_set(loop, &home->breaker, home->until);
	LOG(LOG_WARN, "upstream unavailable: tool calls refused for %" PRIu64 " s",
	    spell / NS);
	loop_ready_bulk(loop, &home->shed);
}

static void
home_breaker_event (struct loop       *loop,
                    struct loop_timer *timer)
{
	struct home *home = container_of(timer, struct home, breaker);
	home_breaker_set(home, HALF);
	LOG(LOG_INFO, "upstream breaker half-open: a probe goes next");
	loop_ready_bulk(loop, &home->start);
}

/* Whether @p job, a background job running, may be cut: not as the probe,
 * nor once its answer is all in. Native background work cannot be restarted
 * this way: closing its stream would leave the old generation running. */
static bool
home_job_cuttable (struct home const *home,
                   struct job const  *job)
{
	return !ai_background(home->ep) && job->transfer &&
	       !(job->flags & (PROBE | DELIVERED | ENDED));
}

/* How many background jobs running hold slots they may not: with one slot,
 * the one an interactive job waits for; else those beyond the S - 1 they
 * may hold, as S may shrink. */
static uint32_t
home_excess (struct home const *home)
{
	if (home->slots == 1 && home->running_bg && !list_is_empty(&home->ring[0]))
		return 1;
	uint32_t may = home_background_slots(home);
	return home->running_bg > may ? home->running_bg - may : 0;
}

/* Have the background jobs that hold slots they may not, but for those being
 * preempted already, preempted, the first to have started first. */
static void
home_preempt_excess (struct home *home)
{
	uint32_t over = home_excess(home);
	struct list *node;
	list_foreach (node, &home->preempting)
		over -= over > 0;
	struct list *next;
	list_foreach_safe (node, next, &home->background) {
		if (!over)
			return;
		struct job *job = container_of(node, struct job, queue);
		/* Once at most. */
		if (!home_job_cuttable(home, job) || job->flags & PREEMPTED)
			continue;
		job->flags |= PREEMPTED;
		list_del(&job->queue);
		list_append(&home->preempting, &job->queue);
		loop_ready_bulk(home->loop, &home->preempt);
		--over;
	}
}

/* The probe, @p job, has gone through, which closes the breaker, unless
 * @p failure, which opens it again, for @p retry seconds or the backoff. */
static void
home_breaker_probed (struct home *home,
                     struct job  *job,
                     bool         failure,
                     long         retry)
{
	job->flags &= (uint16_t)~PROBE;
	home->probing = false;
	if (failure) {
		home_breaker_open(home, retry);
		return;
	}
	home_breaker_set(home, CLOSED);
	home->failures = 0;
	home->backoff = BACKOFF_MIN;
	LOG(LOG_INFO, "upstream breaker closed: the probe went through");
	loop_ready_bulk(home->loop, &home->start);
	/* An interactive job may wait for the probe's slot. */
	home_preempt_excess(home);
}

/* Count how @p job went upstream: a failure in a row of FAILURES, or of the
 * probe, opens the breaker, as does one whose answer said when to retry; the
 * probe's success, which its answer's beginning tells, closes it.  Other
 * jobs' outcomes count only while it is closed, and a job cancelled counts
 * not at all. */
static void
home_breaker_note (struct home *home,
                   struct job  *job,
                   bool         failure,
                   long         retry)
{
	if (job->flags & PROBE) {
		home_breaker_probed(home, job, failure, retry);
		return;
	}
	if (home->state != CLOSED)
		return;
	if (!failure) {
		home->failures = 0;
		return;
	}
	if (++home->failures >= FAILURES || retry > 0)
		home_breaker_open(home, retry);
}

/* Whether @p job's run upstream, ended with @p result, is the upstream's
 * failure: a transfer that failed, but for a write home refused or memory it
 * ran out of, or an answer whose status is a server error or 429. */
static bool
home_job_failure (struct job const *job,
                  CURLcode          result)
{
	if (result)
		return result != CURLE_WRITE_ERROR && result != CURLE_OUT_OF_MEMORY;
	return job->http >= 500 || job->http == 429;
}

/* Say in @p job's result why its run upstream, ended with @p result and
 * libcurl's @p error, failed, unless an error event of its stream has. */
static int
home_explain (struct home *home,
              struct job  *job,
              CURLcode     result,
              char const  *error)
{
	if (job->flags & OVERFLOW)
		return home_job_say(job, "Upstream response exceeds %d MiB.",
		                    MAX_RESPONSE >> 20);
	if (job->flags & STALLED)
		return home_job_say(job, "Upstream answer stalled: nothing came for %.0f s.",
		                    (double)home->silence / 1e9);
	if (result == CURLE_OPERATION_TIMEDOUT)
		return home_job_say(job, "Upstream request timed out after %.0f s.",
		                    (double)(home->loop->now - job->since) / 1e9);
	if (!result)
		return 0;
	return home_job_say(job, "Upstream request failed: %s%s%s", curl_easy_strerror(result),
	                    *error ? ": " : "", error);
}

/* Complete @p job, whose stream has ended whole, with its tool calls and
 * text, or its text, or why it has none in its place: a text cut short says
 * so, while the front tells tool calls cut short. */
static void
home_finish_text (struct home *home,
                  struct job  *job)
{
	_Atomic uint64_t *completed = &home->metrics->completed;
	if (buf_peek(&job->calls[0]).len) {
		home_end(home, job, JOB_TEXT, completed);
		return;
	}
	struct span s;
	if (!buf_peek(&job->result).len) {
		char const *reason = *job->reason ? job->reason : nullptr;
		if (!buf_reserve(&job->result, 96, SIZE_MAX, &s))
			buf_commit(&job->result, ai_no_answer((char *)s.ptr, s.len, reason));
		home_end(home, job, JOB_FAILED, completed);
		return;
	}
	if (job->flags & TRUNCATED &&
	    !buf_reserve(&job->result, sizeof AI_CUT_NOTE - 1, SIZE_MAX, &s)) {
		memcpy(s.ptr, AI_CUT_NOTE, sizeof AI_CUT_NOTE - 1);
		buf_commit(&job->result, sizeof AI_CUT_NOTE - 1);
	}
	home_end(home, job, JOB_TEXT, completed);
}

/* @p job's run upstream is over, with @p result and libcurl's @p error, and
 * @p retry the seconds its answer's Retry-After asked for: complete it with
 * the answer, its text, or why there is none in its place. */
static void
home_finish (struct home *home,
             struct job  *job,
             CURLcode     result,
             char const  *error,
             long         retry)
{
	home_breaker_note(home, job, home_job_failure(job, result), retry);
	if (result || job->flags & ERRED) {
		(void)home_explain(home, job, result, error);
		home_end(home, job, JOB_FAILED, &home->metrics->completed);
		return;
	}
	if (job->flags & STREAM)
		home_finish_text(home, job);
	else
		home_end(home, job, JOB_ANSWERED, &home->metrics->completed);
}

/* End @p job's run upstream now, with @p result, cancelling its transfer if
 * it still has one: the upstream sees a hang-up. */
static void
home_cut (struct home *home,
          struct job  *job,
          CURLcode     result)
{
	http_cancel(home->http, &job->transfer);
	home_finish(home, job, result, "", 0);
}

/* Have the resume step let @p job's answer, held back, on. */
static void
home_hold (struct home *home,
           struct job  *job)
{
	if (list_is_empty(&job->held))
		list_append(&home->held, &job->held);
	loop_ready_bulk(home->loop, &home->resume);
}

/* Have the feed step read @p job's stream. */
static void
home_feed (struct home *home,
           struct job  *job)
{
	if (list_is_empty(&job->feed))
		list_append(&home->feeding, &job->feed);
	loop_ready_bulk(home->loop, &home->feed);
}

/* @p job's answer begins: note its status, and whether it is an event
 * stream.  The upstream answers: the other jobs need not wait for the
 * probe's end, which may be minutes away. */
static void
home_head (struct home *home,
           struct job  *job)
{
	static char const sse[] = "text/event-stream";
	long status;
	char const *type;
	http_response(job->transfer, &status, &type);
	job->flags |= SEEN;
	job->http = (uint16_t)status;
	if (status >= 200 && status < 300 && type && !strncasecmp(type, sse, sizeof sse - 1))
		job->flags |= STREAM;
	if (job->flags & PROBE && !home_job_failure(job, CURLE_OK))
		home_breaker_probed(home, job, false, 0);
}

/* Take the @p len bytes at @p data of @p job's answer into @p to, up to
 * MAX_RESPONSE bytes, as much a step as its share of receiving takes: curl
 * holds the rest back until a later step lets it on (home_resume_step()).
 * A write over a whole share, which curl does not make, would count in full.
 * Returns as a write callback does. */
static size_t
home_receive (struct home *home,
              struct job  *job,
              struct buf  *to,
              char const  *data,
              size_t       len)
{
	struct loop *loop = home->loop;
	uint64_t room = loop_room(loop, LOOP_RECV);
	if (room < len && room < LOOP_STEP_RECV) {
		home_hold(home, job);
		return CURL_WRITEFUNC_PAUSE;
	}
	struct span s;
	int e = buf_reserve(to, len, MAX_RESPONSE, &s);
	if (e) {
		job->flags |= e == ENOBUFS ? OVERFLOW : 0;
		return 0;
	}
	memcpy(s.ptr, data, len);
	buf_commit(to, len);
	loop_work(loop, LOOP_RECV, len);
	return len;
}

/* Gather the upstream's answer, a stream into the wire and a plain one into
 * the result (home_receive()).  A stream is held back too while a step's
 * share of it waits to be read.  Each write counts as the answer's sign of
 * life. */
static size_t
home_job_write (char const *data,
                size_t      len,
                void       *user)
{
	struct job *job = user;
	struct home *home = self;
	if (!len)
		return 0;
	if (!(job->flags & SEEN))
		home_head(home, job);
	job->active = home->loop->now;
	list_del(&job->idle);
	list_append(&home->idle, &job->idle);
	if (home->silence && home->quiet.due == UINT64_MAX)
		(void)loop_timer_set(home->loop, &home->quiet, job->active + home->silence);
	if (!(job->flags & STREAM))
		return home_receive(home, job, &job->result, data, len);
	if (buf_peek(&job->wire).len - job->scan >= LOOP_STEP_RECV) {
		job->flags |= BACKED;
		return CURL_WRITEFUNC_PAUSE;
	}
	size_t n = home_receive(home, job, &job->wire, data, len);
	if (n == len)
		home_feed(home, job);
	return n;
}

/* The upstream call has ended.  A stream's end, once all of it is read; any
 * other answer, or none, at once. */
static void
home_job_done (CURLcode    result,
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
	if (result && job->flags & STREAM && ai_background(home->ep) &&
	    buf_peek(&job->wire).len) {
		job->delivery_error = malloc(sizeof *job->delivery_error);
		if (job->delivery_error) {
			job->delivery_error->result = result;
			job->delivery_error->retry = http_retry_after(home->http);
			util_format(job->delivery_error->message,
			            sizeof job->delivery_error->message, "%s", error);
			job->flags |= DELIVERED;
			home_feed(home, job);
			return;
		}
	}
	if (result == CURLE_OK && job->flags & STREAM) {
		job->flags |= DELIVERED;
		home_feed(home, job);
		return;
	}
	home_finish(home, job, result, error, http_retry_after(home->http));
}

/* Put @p job at the end of the timeline, and have the timer wait for its
 * deadline if it waits for none. */
static void
home_timeline (struct home *home,
               struct job  *job)
{
	list_del(&job->due);
	list_append(&home->timeline, &job->due);
	if (job->deadline && home->deadline.due == UINT64_MAX)
		(void)loop_timer_set(home->loop, &home->deadline, job->deadline);
}

/* Start @p job upstream, at the end of the timeline, its deadline the
 * latest; half-open, as the breaker's probe. */
static void
home_start (struct home *home,
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
	if (home_job_class(job)) {
		list_append(&home->background, &job->queue);
		hist_add(&m->background, 1);
		++home->running_bg;
	}
	job->since = loop->now;
	job->context = home->context;
	job->deadline = home->timeout ? loop->now + home->timeout : 0;
	home_timeline(home, job);
	home_job_progress(job, 0);
	struct span body = buf_peek(&job->request);
	if (http_post_json(home->http, ep->url, (char const *)body.ptr, body.len, false,
	                   ep->headers, 0, home_job_write, home_job_done, job, &job->transfer))
		home_end(home, job, JOB_FAILED, &m->completed);
}

/* The job of class @p c to start next: the first ready, else the oldest of
 * the ring's first principal; nullptr if none waits. */
static struct job *
home_first (struct home const *home,
            uint32_t           c)
{
	if (!list_is_empty(&home->ready[c]))
		return container_of(home->ready[c].next, struct job, queue);
	if (list_is_empty(&home->ring[c]))
		return nullptr;
	struct home_principal *p = container_of(home->ring[c].next, struct home_principal,
	                                        ring);
	return container_of(p->jobs.next, struct job, queue);
}

/* The job to start next: an interactive one, then a background one; but none
 * of the background ones to @p start while they hold all the slots they may;
 * nullptr if none waits. */
static struct job *
home_next (struct home const *home,
           bool               start)
{
	struct job *job = home_first(home, 0);
	if (job || (start && home->running_bg >= home_background_slots(home)))
		return job;
	return home_first(home, 1);
}

/* Start jobs waiting while slots are free, home does not drain, the breaker
 * lets them and the step's share of requests allows: those ready first, then
 * the next principal on the ring starts its first and goes to the back. */
static void
home_start_step (struct loop      *loop,
                 struct loop_step *step)
{
	struct home *home = container_of(step, struct home, start);
	while (!home->draining && home->state != OPEN && !home->probing &&
	       hist_get(&home->metrics->running) < home->slots) {
		struct job *job = home_next(home, true);
		if (!job)
			return;
		if (!loop_room(loop, LOOP_REQS)) {
			loop_ready_bulk(loop, step);
			return;
		}
		if (job->flags & READY) {
			job->flags &= (uint16_t)~READY;
			--home->readied[home_job_class(job)];
			list_del(&job->queue);
		} else {
			list_del(&job->who->ring);
			list_append(&home->ring[home_job_class(job)], &job->who->ring);
			home_unqueue(home, job);
		}
		home_start(home, job);
		loop_work(loop, LOOP_REQS, 1);
	}
}

static void
home_deadline_event (struct loop       *loop,
                     struct loop_timer *timer)
{
	loop_ready_bulk(loop, &container_of(timer, struct home, deadline)->overdue);
}

/* Cancel @p job's transfer, if it has one: one whose request had gone
 * upstream counts as cancelled there. */
static void
home_abort (struct home *home,
            struct job  *job)
{
	if (job->transfer && http_sent(job->transfer))
		hist_add(&home->metrics->aborted, 1);
	http_cancel(home->http, &job->transfer);
}

/* Cancels @p job, which its front dropped or a drain ends. */
static void
home_cancel (struct home *home,
             struct job  *job)
{
	home_abort(home, job);
	home_end(home, job, JOB_CANCELLED, &home->metrics->cancelled);
}

/* End @p job, past its deadline: one running times out, one waiting for a
 * slot does too, refused, and one dropped is cancelled.  libcurl short of
 * memory may lose a transfer's own timeout. */
static void
home_expire (struct home *home,
             struct job  *job)
{
	if (job->flags & DROPPED) {
		home_cancel(home, job);
		return;
	}
	if (job->flags & RUNNING) {
		home_cut(home, job, CURLE_OPERATION_TIMEDOUT);
		return;
	}
	struct home_metrics *m = home->metrics;
	double s = (double)home->ep->timeout_ms / 1e3;
	if (job->flags & PREEMPTED) {
		/* It went upstream, and keeps its deadline from then. */
		(void)home_job_say(job, "Timed out after %.0f s: an interactive call "
		                   "preempted it, and it could not start again in time.", s);
		home_end(home, job, JOB_FAILED, &m->completed);
		return;
	}
	(void)home_job_say(job, "Timed out after %.0f s waiting for an upstream slot.", s);
	home_end(home, job, JOB_FAILED, &m->rejected);
}

/* End the jobs past their deadline, oldest first, as many a step as its share
 * of requests allows (home_expire()); then wait for the next one's. */
static void
home_overdue_step (struct loop      *loop,
                   struct loop_step *step)
{
	struct home *home = container_of(step, struct home, overdue);
	for (; !list_is_empty(&home->timeline); loop_work(loop, LOOP_REQS, 1)) {
		struct job *job = container_of(home->timeline.next, struct job, due);
		uint64_t due = job->deadline ? job->deadline : UINT64_MAX;
		if (due > loop->now) {
			(void)loop_timer_set(loop, &home->deadline, due);
			return;
		}
		if (!loop_room(loop, LOOP_REQS)) {
			loop_ready_bulk(loop, step);
			return;
		}
		home_expire(home, job);
	}
	(void)loop_timer_set(loop, &home->deadline, UINT64_MAX);
}

static void
home_quiet_event (struct loop       *loop,
                  struct loop_timer *timer)
{
	loop_ready_bulk(loop, &container_of(timer, struct home, quiet)->silent);
}

/* End the answers that sent nothing for the chunk timeout, the one silent
 * longest first, as many a step as its share of requests allows; then wait
 * for the next. */
static void
home_silent_step (struct loop      *loop,
                  struct loop_step *step)
{
	struct home *home = container_of(step, struct home, silent);
	for (; !list_is_empty(&home->idle); loop_work(loop, LOOP_REQS, 1)) {
		struct job *job = container_of(home->idle.next, struct job, idle);
		uint64_t due = job->active + home->silence;
		if (due > loop->now) {
			(void)loop_timer_set(loop, &home->quiet, due);
			return;
		}
		if (!loop_room(loop, LOOP_REQS)) {
			loop_ready_bulk(loop, step);
			return;
		}
		job->flags |= STALLED;
		hist_add(&home->metrics->stalls, 1);
		home_cut(home, job, CURLE_OPERATION_TIMEDOUT);
	}
	(void)loop_timer_set(loop, &home->quiet, UINT64_MAX);
}

/* Let the answers held back on, oldest first, while the step's share of
 * receiving has room for a write from curl, which may hold one back again;
 * curl reads on in a later step. */
static void
home_resume_step (struct loop      *loop,
                  struct loop_step *step)
{
	struct home *home = container_of(step, struct home, resume);
	while (!list_is_empty(&home->held)) {
		if (loop_room(loop, LOOP_RECV) < CURL_MAX_WRITE_SIZE) {
			loop_ready_bulk(loop, step);
			return;
		}
		struct job *job = container_of(home->held.next, struct job, held);
		list_del(&job->held);
		CURLcode result = http_resume(job->transfer);
		if (result)
			home_cut(home, job, result);
	}
}

/* Add the @p len bytes of text at @p text to @p to, a job's answer, its
 * thinking or its replay, if they are UTF-8 (json-c lets overlong forms through), and the
 * buffer stays within MAX_RESPONSE; it is copied whole. */
static int
home_append_text (struct home *home,
                  struct buf  *to,
                  char const  *text,
                  size_t       len)
{
	if (jb_utf8(text, len) < len)
		return EILSEQ;
	if (!len)
		return 0;
	struct span s;
	int e = buf_reserve(to, len, MAX_RESPONSE, &s);
	if (e)
		return e;
	memcpy(s.ptr, text, len);
	buf_commit(to, len);
	loop_work(home->loop, LOOP_COPY, len);
	return 0;
}

/* The answer of the job @p user gains the @p len bytes of text at @p text
 * (home_append_text()). */
static int
home_put_text (void       *user,
               char const *text,
               size_t      len)
{
	struct job *job = user;
	return home_append_text(self, &job->result, text, len);
}

/* Its thinking, likewise. */
static int
home_put_thought (void       *user,
                  char const *text,
                  size_t      len)
{
	struct job *job = user;
	return home_append_text(self, &job->thought, text, len);
}

/* Its replay, likewise: JSON text. */
static int
home_put_replay (void       *user,
                 char const *json,
                 size_t      len)
{
	struct job *job = user;
	return home_append_text(self, &job->replay, json, len);
}

/* Add the piece @p call of a tool call to the calls of the job @p user.
 * Call k is stored in job->calls[k], and the calls are numbered in the
 * order in which they begin.  A piece with an index that no stored call has
 * begins a new call.  A piece without an index begins a new call if there
 * is no call yet or if its id differs from the id of the last call.
 * Otherwise the piece belongs to the last call.  The pieces of different
 * calls may come in any order.  In each call, the id comes before the name,
 * and the name comes before the extra.  The extra comes once and before the
 * arguments.  The arguments may come in several pieces.  The function
 * ignores an id, a name or an extra that repeats the stored bytes, because
 * some servers send them again.  The function returns EPROTO if a call
 * breaks this order or if there are more than JOB_CALLS calls.  It returns
 * ENOBUFS if the calls together would have more than MAX_RESPONSE bytes,
 * and EILSEQ if a piece is not UTF-8. */
static int
home_put_call (void                 *user,
               struct ai_call const *call)
{
	struct job *job = user;
	/* This loop counts the calls in n and the bytes of all calls in bytes.
	 * It sets k to the number of the call that has the index of the
	 * piece, and c to the header of the last call. */
	struct job_call c = {};
	uint32_t n = 0;
	uint32_t k = JOB_CALLS;
	size_t bytes = 0;
	for (; n < JOB_CALLS; ++n) {
		struct span s = buf_peek(&job->calls[n]);
		if (!s.len)
			break;
		memcpy(&c, s.ptr, sizeof c);
		k = c.index == call->index ? n : k;
		bytes += s.len;
	}
	size_t id = call->id_len;
	size_t name = call->name_len;
	size_t extra = call->extra_len;
	size_t args = call->args_len;
	uint32_t index = call->index;
	/* A piece without an index continues the last call if there is a
	 * call and the piece has no id or the same id as the last call.  Then
	 * its id is not stored again.  Otherwise the piece begins call n.  No
	 * stored call has the index AI_CALL_NEXT, so k is still JOB_CALLS for
	 * such a piece. */
	if (index == AI_CALL_NEXT) {
		bool same = n && !(id && (id != c.id ||
		                          memcmp(buf_peek(&job->calls[n - 1]).ptr + sizeof c,
		                                 call->id, id)));
		index = n;
		if (same) {
			k = n - 1;
			id = 0;
		}
	}
	size_t head = 0;
	if (k < n) {
		/* Ignore an id, a name or an extra that repeats the stored
		 * bytes. */
		unsigned char const *had = buf_peek(&job->calls[k]).ptr;
		memcpy(&c, had, sizeof c);
		had += sizeof c;
		if (id && id == c.id && !memcmp(had, call->id, id))
			id = 0;
		had += c.id;
		if (name && name == c.name && !memcmp(had, call->name, name))
			name = 0;
		had += c.name;
		if (extra && extra == c.extra && !memcmp(had, call->extra, extra))
			extra = 0;
	} else if (n == JOB_CALLS) {
		return EPROTO;
	} else {
		c = (struct job_call){
			.index = index,
			.replay = (uint32_t)buf_peek(&job->replay).len
		};
		head = sizeof c;
		k = n;
	}
	bool later = c.extra || c.args;
	if ((id && (c.name || later)) || ((name || extra) && later))
		return EPROTO;
	if (jb_utf8(call->id, id) < id || jb_utf8(call->name, name) < name ||
	    jb_utf8(call->extra, extra) < extra || jb_utf8(call->args, args) < args)
		return EILSEQ;
	size_t len = head + id + name + extra + args;
	if (!len)
		return 0;
	/* The limit of the buffer is MAX_RESPONSE minus the bytes of the
	 * other calls, so the calls together stay within MAX_RESPONSE. */
	struct buf *to = &job->calls[k];
	struct span s;
	int e = buf_reserve(to, len, MAX_RESPONSE - (bytes - buf_peek(to).len), &s);
	if (e)
		return e;
	/* After the order check above, the new bytes belong to the last
	 * stored part of the call or to the parts after it, so they go to the
	 * end of the buffer. */
	unsigned char *p = s.ptr + head;
	if (id)
		memcpy(p, call->id, id);
	if (name)
		memcpy(p + id, call->name, name);
	if (extra)
		memcpy(p + id + name, call->extra, extra);
	if (args)
		memcpy(p + id + name + extra, call->args, args);
	c.id += (uint32_t)id;
	c.name += (uint32_t)name;
	c.extra += (uint32_t)extra;
	c.args += (uint32_t)args;
	buf_commit(to, len);
	memcpy(buf_peek(to).ptr, &c, sizeof c);
	loop_work(self->loop, LOOP_COPY, len);
	return 0;
}

/* Parse on with @p parser, the @p len bytes at @p json, a slice as far as
 * the step's shares allow, which it counts; true once the parse is over. */
static bool
home_parse (struct home      *home,
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
	bool over = jb_parser_step(parser, &home->pool, json, len, bytes, (uint32_t)values);
	loop_work(loop, LOOP_PARSE, parser->off - off);
	loop_work(loop, LOOP_VALUES, parser->cut.values - seen);
	loop_work(loop, LOOP_COPY, parser->cut.copied - copied);
	return over;
}

/* Build the cancellation URL from our endpoint and a single escaped id.
 * In particular, a provider's id cannot replace the host or query. */
static int
home_native_id (struct home        *home,
                struct job         *job,
                struct json_object *value)
{
	size_t len = 0;
	char const *id = jb_str(value, &len);
	if (!id || !len || len > MAX_JOB_ID || memchr(id, '\0', len) ||
	    (len == 1 && *id == '.') || (len == 2 && !memcmp(id, "..", 2)))
		return EPROTO;
	CURLU *url = curl_url();
	char *escaped = curl_easy_escape(nullptr, id, (int)len);
	char *path = nullptr;
	char *joined = nullptr;
	int e = ENOMEM;
	if (!url || !escaped)
		goto done;
	if (curl_url_set(url, CURLUPART_URL, home->ep->url, 0) ||
	    curl_url_get(url, CURLUPART_PATH, &path, 0)) {
		e = EINVAL;
		goto done;
	}
	size_t at = strlen(path);
	while (at && path[at - 1] == '/')
		--at;
	size_t n = strlen(escaped);
	static char const suffix[] = "/cancel";
	if (at > SIZE_MAX - n - sizeof suffix - 1)
		goto done;
	joined = malloc(at + 1 + n + sizeof suffix);
	if (!joined)
		goto done;
	memcpy(joined, path, at);
	joined[at++] = '/';
	memcpy(joined + at, escaped, n);
	memcpy(joined + at + n, suffix, sizeof suffix);
	if (curl_url_set(url, CURLUPART_PATH, joined, 0) ||
	    curl_url_get(url, CURLUPART_URL, &job->cancel_url, 0)) {
		e = EINVAL;
		goto done;
	}
	e = 0;
done:
	free(joined);
	joined = nullptr;
	curl_free(path);
	path = nullptr;
	curl_free(escaped);
	escaped = nullptr;
	curl_url_cleanup(url);
	return e;
}

/* Capture the native creation id while its event is alive, and remember
 * terminal provider states so cleanup does not cancel completed work. */
static int
home_native_event (struct home        *home,
                   struct job         *job,
                   struct json_object *root)
{
	if (!ai_background(home->ep))
		return 0;
	bool responses = home->ep->api == AI_RESPONSES;
	struct json_object *kind = jb_get(root, responses ? "type" : "event_type");
	struct json_object *object = jb_get(root, responses ? "response" : "interaction");
	struct json_object *status = jb_get(object, "status");
	if (!responses && jb_is(kind, "interaction.status_update"))
		status = jb_get(root, "status");
	if (jb_is(status, "completed") || jb_is(status, "failed") ||
	    jb_is(status, "cancelled") || jb_is(status, "incomplete"))
		job->provider_done = true;
	if (job->cancel_url || !jb_is(kind, responses ? "response.created"
	                                            : "interaction.created"))
		return 0;
	return home_native_id(home, job, jb_get(object, "id"));
}

/* Take what @p root, an event of @p job's stream, says (ai_event()): its
 * text, and with JOB_TOOLS its thinking and tool calls; its usage, its end,
 * and its stop reason, which may be the output token limit.  Each event is
 * a chunk, and one with text or thinking a token.  An error it says, or one
 * that taking it meets of the answer, goes in the result.  Returns the
 * error that taking it met: one past MAX_RESPONSE is an overflow. */
static int
home_take (struct home        *home,
           struct job         *job,
           struct json_object *root)
{
	int e = home_native_event(home, job, root);
	if (e) {
		(void)home_job_erred(job, "cannot retain the native background job id", -1);
		return e;
	}
	static struct ai_sink const plain = { .text = home_put_text };
	static struct ai_sink const tools = {
		.text = home_put_text,
		.thought = home_put_thought,
		.call = home_put_call,
		.replay = home_put_replay
	};
	struct ai_sink sink = job->asks & JOB_TOOLS ? tools : plain;
	sink.user = job;
	size_t had = buf_peek(&job->result).len;
	struct ai_event ev = {};
	e = ai_event(home->ep->api, &job->reader, root, &sink, &ev);
	hist_add(&home->metrics->chunks, 1);
	if (buf_peek(&job->result).len > had)
		home_job_progress(job, (uint64_t)++job->chunks << 32);
	else if (ev.thought)
		home_job_progress(job, (uint64_t)++job->chunks << 32 | JOB_THINKING);
	job->used = ev.used ? ev.used : job->used;
	job->provider_done |= ev.end != 0;
	job->flags |= (ev.end ? ENDED : 0) | (e == ENOBUFS ? OVERFLOW : 0);
	if (ev.reason) {
		size_t len = jb_utf8(ev.reason, strnlen(ev.reason, sizeof job->reason - 1));
		memcpy(job->reason, ev.reason, len);
		job->reason[len] = '\0';
		job->flags = (uint16_t)(ev.truncated ? job->flags | TRUNCATED
		                                     : job->flags & ~TRUNCATED);
	}
	if (ev.error) {
		size_t len = strnlen(ev.error, 4096);
		(void)home_job_erred(job, ev.error, (int)jb_utf8(ev.error, len));
	} else if (e == EPROTO) {
		(void)home_job_erred(job, "tool calls out of order, or too many", -1);
	} else if (e == EILSEQ) {
		(void)home_job_erred(job, "an answer that is not text", -1);
	}
	return e;
}

/* Take the event parsed in @p job's parser (home_take()), and free its
 * value, the reap step what the step's share does not allow; returns the
 * error that taking it met: ENOMEM if parsing it ran out of memory.  An
 * event that is not JSON text errs. */
static int
home_event (struct home *home,
            struct job  *job)
{
	struct loop *loop = home->loop;
	struct jb_parser *parser = &job->parser;
	uint32_t n = parser->cut.values;
	struct json_object *root = jb_parser_take(parser);
	if (!root) {
		struct json_tokener *tok = parser->tok;
		bool memory = !tok ||
		              json_tokener_get_error(tok) == json_tokener_error_memory;
		jb_parser_fini(parser, &home->pool, &home->reaper);
		home_reap(home);
		if (memory)
			return ENOMEM;
		(void)home_job_erred(job, "an event that is not JSON text", -1);
		return 0;
	}
	jb_parser_fini(parser, &home->pool, &home->reaper);
	int e = home_take(home, job, root);
	if (n <= loop_room(loop, LOOP_VALUES)) {
		json_object_put(root);
		loop_work(loop, LOOP_VALUES, n);
	} else {
		jb_reaper_take(&home->reaper, root);
	}
	home_reap(home);
	return e;
}

/* Parse the event whole in @p job's wire, a slice a step, take what it says,
 * and free it; then go on past it.  0 once it has, EAGAIN if the step's
 * share ran out first, ECANCELED if the event ended the job.  An event that
 * is not JSON text ends it, as a whole answer that is not would.  The data
 * [DONE], which is not JSON text, is not parsed.  It ends the answer,
 * except in Gemini's Interactions, where it is skipped, because only
 * interaction.completed ends the answer there. */
static int
home_read_event (struct home *home,
                 struct job  *job)
{
	static char const done[] = "[DONE]";
	char const *data = (char const *)buf_peek(&job->wire).ptr + job->ev;
	int e = 0;
	if (job->evlen == sizeof done - 1 && !memcmp(data, done, sizeof done - 1))
		job->flags |= home->ep->api == AI_INTERACTIONS ? 0 : ENDED;
	else if (!home_parse(home, &job->parser, data, job->evlen))
		return EAGAIN;
	else
		e = home_event(home, job);
	buf_consume(&job->wire, job->line);
	job->ev = job->evlen = job->line = job->scan = 0;
	job->flags &= (uint16_t)~EVENT;
	if (job->flags & ERRED) {
		home_cut(home, job, CURLE_OK);
		return ECANCELED;
	}
	if (!e)
		return 0;
	home_cut(home, job, e == ENOBUFS ? CURLE_WRITE_ERROR : CURLE_OUT_OF_MEMORY);
	return ECANCELED;
}

/* Take in the line of @p job's stream that ends at @p end, its line break:
 * a data line's data joins the event's, moved down to follow it after a
 * line break, and a blank line ends the event, which has some; every other
 * line says nothing, and goes once no event's data precedes it (SSE). */
static void
home_job_line (struct job *job,
               size_t      end)
{
	static char const field[] = "data:";
	unsigned char *wire = buf_peek(&job->wire).ptr;
	unsigned char *line = wire + job->line;
	size_t len = end - job->line;
	len -= len && line[len - 1] == '\r';
	size_t skip = sizeof field - 1;
	skip += len > skip && line[skip] == ' ';
	uint32_t at = job->line + (uint32_t)skip;
	uint32_t n = len > skip && !memcmp(line, field, sizeof field - 1)
	           ? (uint32_t)(len - skip) : 0;
	job->line = job->scan;
	if (!job->evlen) {
		job->ev = at;
		job->evlen = n;
		if (n)
			return;
		buf_consume(&job->wire, job->line);
		job->line = job->scan = 0;
		return;
	}
	if (!n) {
		job->flags |= len ? 0 : EVENT;
		return;
	}
	wire[job->ev + job->evlen++] = '\n';
	memmove(wire + job->ev + job->evlen, wire + at, n);
	job->evlen += n;
}

/* Look for the line break that ends the line of @p job's stream read, as far
 * as the step's share of parsing allows, and take in the line it ends.  0
 * once it has looked, EAGAIN if the share has run out, ENODATA if all of the
 * wire has been looked at already. */
static int
home_scan (struct home *home,
           struct job  *job)
{
	struct span w = buf_peek(&job->wire);
	if (job->scan >= w.len)
		return ENODATA;
	struct loop *loop = home->loop;
	uint64_t room = loop_room(loop, LOOP_PARSE);
	if (!room)
		return EAGAIN;
	size_t n = w.len - job->scan < room ? w.len - job->scan : room;
	unsigned char const *nl = memchr(w.ptr + job->scan, '\n', n);
	if (!nl) {
		loop_work(loop, LOOP_PARSE, n);
		job->scan += (uint32_t)n;
		return 0;
	}
	size_t end = (size_t)(nl - w.ptr);
	loop_work(loop, LOOP_PARSE, end + 1 - job->scan);
	job->scan = (uint32_t)end + 1;
	home_job_line(job, end);
	return 0;
}

/* All of @p job's wire has been looked at.  A stream that libcurl has not
 * delivered whole has nothing more to read for now (ENODATA): it is off the
 * list of streams to read, and goes on if it was held back for reading.  One
 * delivered ends with its last event read, the last line and the last event
 * needing no line breaks (0 until then): cut off, as a transfer cut short,
 * unless an event said the answer had ended (ECANCELED). */
static int
home_drained (struct home *home,
              struct job  *job)
{
	if (!(job->flags & DELIVERED)) {
		list_del(&job->feed);
		if (job->flags & BACKED) {
			job->flags &= (uint16_t)~BACKED;
			home_hold(home, job);
		}
		return ENODATA;
	}
	if (job->line < job->scan) {
		home_job_line(job, job->scan);
		return 0;
	}
	if (job->evlen) {
		job->flags |= EVENT;
		return 0;
	}
	if (job->delivery_error) {
		struct home_delivery_error const *error = job->delivery_error;
		home_finish(home, job, error->result, error->message, error->retry);
	} else if (job->flags & ENDED)
		home_finish(home, job, CURLE_OK, "", 0);
	else
		home_finish(home, job, CURLE_PARTIAL_FILE,
		            "the stream ended before the answer did", 0);
	return ECANCELED;
}

/* Read @p job's stream as far as the step's shares allow, its lines and each
 * event whole, until it has nothing more to read for now, or has ended
 * (home_drained()): true then, false if the shares ran out first. */
static bool
home_read_stream (struct home *home,
                  struct job  *job)
{
	for (;;) {
		int e = job->flags & EVENT ? home_read_event(home, job) : home_scan(home, job);
		if (e == ENODATA)
			e = home_drained(home, job);
		if (e)
			return e != EAGAIN;
	}
}

/* Read the streams, each as far as the step's shares allow, the one that
 * used them up going to the back for the next step. */
static void
home_feed_step (struct loop      *loop,
                struct loop_step *step)
{
	struct home *home = container_of(step, struct home, feed);
	while (!list_is_empty(&home->feeding)) {
		struct job *job = container_of(home->feeding.next, struct job, feed);
		if (home_read_stream(home, job))
			continue;
		list_del(&job->feed);
		list_append(&home->feeding, &job->feed);
		loop_ready_bulk(loop, step);
		return;
	}
}

static void
home_reap_step (struct loop      *loop,
                struct loop_step *step)
{
	struct home *home = container_of(step, struct home, reap);
	loop_work(loop, LOOP_VALUES, jb_reaper_step(&home->reaper,
	                                            loop_room(loop, LOOP_VALUES)));
	home_reap(home);
}

/* The breaker opened: end the jobs waiting, in the order they would have
 * started, as many a step as its share of requests allows. */
static void
home_shed_step (struct loop      *loop,
                struct loop_step *step)
{
	struct home *home = container_of(step, struct home, shed);
	while (home->state == OPEN) {
		struct job *job = home_next(home, false);
		if (!job)
			return;
		if (!loop_room(loop, LOOP_REQS)) {
			loop_ready_bulk(loop, step);
			return;
		}
		home_refuse(home, job);
		loop_work(loop, LOOP_REQS, 1);
	}
}

/* Cut the upstream call of @p job, a background job that an interactive one
 * preempts, and have it wait again, ready, first of the background jobs, its
 * deadline as it was.  A preemption is no outcome of the upstream's, which
 * the breaker counts. */
static void
home_preempt (struct home *home,
              struct job  *job)
{
	struct home_metrics *m = home->metrics;
	home_abort(home, job);
	home_unstream(home, job);
	buf_consume(&job->result, buf_peek(&job->result).len);
	for (size_t k = 0; k < JOB_CALLS; ++k)
		buf_consume(&job->calls[k], buf_peek(&job->calls[k]).len);
	buf_consume(&job->thought, buf_peek(&job->thought).len);
	buf_consume(&job->replay, buf_peek(&job->replay).len);
	home_job_reset(job, PREEMPTED | READY);
	hist_add(&m->running, -1ULL);
	hist_add(&m->background, -1ULL);
	hist_add(&m->preempted, 1);
	--home->running_bg;
	list_append(home->ready[1].next, &job->queue);
	++home->readied[1];
	home_job_progress(job, 1);
	loop_ready_bulk(home->loop, &home->start);
}

/* Preempt the background jobs that interactive ones preempt, as many a step
 * as its share of requests allows (home_preempt()). */
static void
home_preempt_step (struct loop      *loop,
                   struct loop_step *step)
{
	struct home *home = container_of(step, struct home, preempt);
	for (; !list_is_empty(&home->preempting); loop_work(loop, LOOP_REQS, 1)) {
		if (!loop_room(loop, LOOP_REQS)) {
			loop_ready_bulk(loop, step);
			return;
		}
		struct job *job = container_of(home->preempting.next, struct job, queue);
		list_del(&job->queue);
		/* Its answer may be all in by now, or the interactive job gone. */
		if (home_job_cuttable(home, job) && home_excess(home)) {
			home_preempt(home, job);
			continue;
		}
		job->flags &= (uint16_t)~PREEMPTED;
		list_append(&home->background, &job->queue);
	}
}

/* Cancel the jobs dropped, as many a step as its share of requests
 * allows. */
static void
home_stop_step (struct loop      *loop,
                struct loop_step *step)
{
	struct home *home = container_of(step, struct home, stop);
	for (; !list_is_empty(&home->dropped); loop_work(loop, LOOP_REQS, 1)) {
		if (!loop_room(loop, LOOP_REQS)) {
			loop_ready_bulk(loop, step);
			return;
		}
		home_cancel(home, container_of(home->dropped.next, struct job, queue));
	}
}

/* Draining: cancel every job in flight, likewise. */
static void
home_drain_step (struct loop      *loop,
                 struct loop_step *step)
{
	struct home *home = container_of(step, struct home, drain);
	for (; !list_is_empty(&home->timeline); loop_work(loop, LOOP_REQS, 1)) {
		if (!loop_room(loop, LOOP_REQS)) {
			loop_ready_bulk(loop, step);
			return;
		}
		home_cancel(home, container_of(home->timeline.next, struct job, due));
	}
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
	home_preempt_excess(home);
}

/* Gather /props's answer, up to MAX_PROPS bytes, as much a step as its share
 * of receiving takes, the rest held back for the props step. */
static size_t
home_props_write (char const *data,
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
	struct span s;
	if (buf_reserve(&home->answer, len, MAX_PROPS, &s))
		return 0;
	memcpy(s.ptr, data, len);
	buf_commit(&home->answer, len);
	loop_work(home->loop, LOOP_RECV, len);
	return len;
}

/* /props is over: take @p n slots, saying @p why, unless told the slots,
 * which take all the connections again, /props having had one of its
 * own. */
static void
home_props_end (struct home *home,
                uint32_t     n,
                char const  *why)
{
	buf_fini(&home->answer);
	if (home->ep->slots)
		http_connections(home->http, home->slots);
	else
		home_slots(home, n, why);
}

/* /props, or the model of Gemini's API, failed, as @p fmt says, which is
 * logged: the default slots (home_props_end()). */
[[gnu::format(printf, 2, 3)]]
static void
home_props_fail (struct home *home,
                 char const  *fmt,
                 ...)
{
	char why[64 + CURL_ERROR_SIZE];
	va_list ap;
	va_start(ap, fmt);
	util_vformat(why, sizeof why, fmt, ap);
	va_end(ap);
	LOG(LOG_INFO, "upstream %s: %s", ai_props(home->ep) ? "/props" : "model", why);
	home_props_end(home, SLOTS, "the default");
}

static void
home_props_done (CURLcode    result,
                 long        status,
                 char const *error,
                 void       *user)
{
	struct home *home = user;
	home->ask = nullptr;
	home->paused = false;
	if (result)
		home_props_fail(home, "%s",
		                *error ? error : curl_easy_strerror(result));
	else if (status != 200)
		home_props_fail(home, "HTTP %ld", status);
	else
		loop_ready_bulk(home->loop, &home->props);
}

/* Let /props's answer on, once the step has the share for it, or parse it a
 * slice a step, and take the slots its total_slots says, unless told, and
 * the context its slots' n_ctx says, likewise.  The answer of Gemini's
 * model says the context in inputTokenLimit, and no slots. */
static void
home_props_step (struct loop      *loop,
                 struct loop_step *step)
{
	struct home *home = container_of(step, struct home, props);
	if (home->paused) {
		home->paused = false;
		CURLcode result = http_resume(home->ask);
		if (result)
			http_end(home->http, &home->ask, result);
		return;
	}
	struct jb_parser *parser = &home->parser;
	struct span json = buf_peek(&home->answer);
	if (!home_parse(home, parser, json.len ? (char const *)json.ptr : "", json.len)) {
		loop_ready_bulk(loop, step);
		return;
	}
	struct json_object *root = jb_parser_take(parser);
	jb_parser_fini(parser, &home->pool, &home->reaper);
	bool props = ai_props(home->ep);
	struct json_object *total = jb_get(root, "total_slots");
	struct json_object *ctx = props ? jb_get(jb_get(root, "default_generation_settings"),
	                                         "n_ctx")
	                                : jb_get(root, "inputTokenLimit");
	int64_t n = json_object_is_type(total, json_type_int)
	          ? json_object_get_int64(total) : 0;
	int64_t tokens = json_object_is_type(ctx, json_type_int)
	               ? json_object_get_int64(ctx) : 0;
	jb_reaper_take(&home->reaper, root);
	home_reap(home);
	if (tokens > 0 && !home->context) {
		home->context = tokens < INT32_MAX ? (uint32_t)tokens : INT32_MAX;
		LOG(LOG_INFO, "upstream context: %" PRIu32 " tokens (%s)", home->context,
		    props ? "llama-server's /props" : "the model's inputTokenLimit");
	}
	if (!props)
		home_props_end(home, SLOTS, "the default");
	else if (n > 0)
		home_props_end(home, n < UINT16_MAX ? (uint32_t)n : UINT16_MAX,
		               "llama-server's /props");
	else
		home_props_fail(home, "no total_slots");
}

/* Ask for /props at the server of @p u, the upstream's URL; the answer may
 * take PROPS_MS, or the calls' timeout if shorter.  0 once asked, else the
 * error that kept it from asking. */
static int
home_props_get (struct home *home,
                CURLU       *u)
{
	struct ai_endpoint const *ep = home->ep;
	char *url = nullptr;
	if (curl_url_set(u, CURLUPART_URL, ep->url, 0) ||
	    curl_url_set(u, CURLUPART_PATH, "/props", 0) ||
	    curl_url_set(u, CURLUPART_QUERY, nullptr, 0) ||
	    curl_url_set(u, CURLUPART_FRAGMENT, nullptr, 0) ||
	    curl_url_get(u, CURLUPART_URL, &url, 0))
		return EINVAL;
	long ms = ep->timeout_ms > 0 && ep->timeout_ms < PROPS_MS ? ep->timeout_ms : PROPS_MS;
	int e = http_post_json(home->http, url, nullptr, 0, false, ep->headers, ms,
	                       home_props_write, home_props_done, home, &home->ask);
	curl_free(url);
	return e;
}

/* Ask Gemini's API what the context of the model holds, unless it is told:
 * GET models/MODEL at the root of the API of the upstream's URL, whose
 * answer the props step reads.  0 once asked, ENOTSUP if the upstream has
 * no model or no such URL, else the error that kept it from asking. */
static int
home_model_get (struct home *home)
{
	struct ai_endpoint const *ep = home->ep;
	char *url = home->context ? nullptr : ai_model_url(ep);
	if (!url)
		return ENOTSUP;
	long ms = ep->timeout_ms > 0 && ep->timeout_ms < PROPS_MS ? ep->timeout_ms : PROPS_MS;
	int e = http_post_json(home->http, url, nullptr, 0, false, ep->headers, ms,
	                       home_props_write, home_props_done, home, &home->ask);
	free(url);
	return e;
}

/* Ask the upstream how many slots it has, as llama-server says at GET /props
 * of its endpoint's server (home_props_get()), unless it is known to be no
 * llama-server: a provider's but llama's, or one that has one of Gemini's
 * APIs, which is asked for the context of its model instead
 * (home_model_get()); 0 once asked, else ENOTSUP or the error that kept it
 * from asking. */
static int
home_props_ask (struct home *home)
{
	if (!ai_props(home->ep))
		return home_model_get(home);
	CURLU *u = curl_url();
	if (!u)
		return ENOMEM;
	int e = home_props_get(home, u);
	curl_url_cleanup(u);
	return e;
}

/* Have the stop step cancel @p job, which runs upstream. */
static void
home_drop (struct home *home,
           struct job  *job)
{
	list_del(&job->queue);
	list_append(&home->dropped, &job->queue);
	loop_ready_bulk(home->loop, &home->stop);
}

/* Puts @p job on the timeline.  The job is then in flight. */
static void
home_register (struct home *home,
               struct job  *job)
{
	home_timeline(home, job);
	hist_add(&home->metrics->in_flight, 1);
}

/* Whether a slot is free for a job of class @p c at once: the breaker
 * closed, no job of its class or before it waiting, and, for a background
 * job, the background ones holding fewer slots than they may. */
static bool
home_vacant (struct home const *home,
             uint32_t           c)
{
	return home->state == CLOSED && list_is_empty(&home->ring[0]) &&
	       hist_get(&home->metrics->running) + home->readied[0] + home->readied[1] <
	       home->slots &&
	       (!c || (list_is_empty(&home->ring[1]) &&
	               home->running_bg + home->readied[1] < home_background_slots(home)));
}

/* Admit @p job of class @p c to the queue of its principal, whose entry
 * @p slot holds, or else a new one, kept or allocated, at the back of its
 * class's ring; ENOMEM if out of memory.  An interactive job that waits with
 * one slot preempts a background one. */
static int
home_wait (struct home            *home,
           struct job             *job,
           uint32_t                c,
           struct home_principal **slot)
{
	struct home_principal *p = *slot;
	if (!p) {
		struct list *spare = shelf_take(&home->spare);
		p = spare ? container_of(spare, struct home_principal, ring)
		          : malloc(sizeof *p);
		if (!p)
			return ENOMEM;
		*p = (struct home_principal){ .hash = job->principal, .class = c };
		list_init(&p->jobs);
		*slot = p;
		list_append(&home->ring[c], &p->ring);
		hist_add(&home->metrics->principals, 1);
	}
	home_register(home, job);
	list_append(&p->jobs, &job->queue);
	job->who = p;
	hist_add(&home->metrics->queued, 1);
	home_job_progress(job, ++p->queued);
	if (!c)
		home_preempt_excess(home);
	return 0;
}

/* Admit @p job, or end it at once: draining, with the breaker open, with
 * --max-calls in flight, or with JOB_QUEUED of its class queued for its
 * principal (unless all of a class share one queue, first in first out).
 * If a slot is free for it (home_vacant()), it is ready, queued for none, so
 * that a burst the slots free take at once is not refused; else it waits
 * (home_wait()). */
static void
home_admit (struct home *home,
            struct job  *job)
{
	struct home_metrics *m = home->metrics;
	if (home->draining) {
		home_end(home, job, JOB_CANCELLED, &m->rejected);
		return;
	}
	if (home->state == OPEN) {
		home_refuse(home, job);
		return;
	}
	struct home_principal **slot = home_find_whom(home, job);
	if (hist_get(&m->in_flight) >= home->max ||
	    (*slot && !home->fifo && (*slot)->queued >= JOB_QUEUED)) {
		home_end(home, job, JOB_BUSY, &m->rejected);
		return;
	}
	uint32_t c = home_job_class(job);
	if (home_vacant(home, c)) {
		home_register(home, job);
		list_append(&home->ready[c], &job->queue);
		job->flags |= READY;
		++home->readied[c];
	} else if (home_wait(home, job, c, slot)) {
		home_end(home, job, JOB_FAILED, &m->rejected);
		return;
	}
	loop_ready_bulk(home->loop, &home->start);
	/* It may need a descriptor upstream. */
	httpd_home_crowd(home->door);
}

/* @p job comes, with @p bits: a job may come back for another run, and what
 * home noted of the last goes before any end.  Home admits it, or ends it
 * cancelled if its front has let go of it already, and lets go too. */
static void
home_submit (struct home *home,
             struct job  *job,
             uint32_t     bits)
{
	struct home_metrics *m = home->metrics;
	home_job_reset(job, 0);
	hist_add(&m->submitted, 1);
	if (bits & JOB_CANCEL) {
		home_end(home, job, JOB_CANCELLED, &m->cancelled);
		home_job_put(job, 2);
		return;
	}
	home_admit(home, job);
	home_job_put(job, 1);
}

/* @p job's front lets go of it: home cancels it if it is in flight, and lets
 * go too, the result's storage back to the stash it came from.  Cancelling
 * one running takes a step, which then lets go of it. */
static void
home_release (struct home *home,
              struct job  *job)
{
	if (job->transfer) {
		job->flags |= DROPPED;
		home_drop(home, job);
		home_job_put(job, 1);
		return;
	}
	if (list_is_empty(&job->due))
		home_job_drop_answer(job);
	else
		home_end(home, job, JOB_CANCELLED, &home->metrics->cancelled);
	home_job_put(job, 2);
}

/* @p job comes, with @p bits, after home's loop has stopped: it ends at once
 * if submitted, and nothing counts. */
static void
home_job_orphan (struct job *job,
                 uint32_t    bits)
{
	if (bits & JOB_SUBMIT) {
		home_job_reset(job, 0);
		home_job_end(job, JOB_CANCELLED);
		home_job_put(job, bits & JOB_CANCEL ? 2 : 1);
		return;
	}
	/* Its front has dropped it. */
	home_job_drop_answer(job);
	home_job_put(job, 2);
}

void
home_job (struct loop *loop,
          struct msg  *msg)
{
	struct job *job = container_of(msg, struct job, to_home);
	uint32_t bits = atomic_exchange(&job->home_bits, 0);
	if (loop->exit) {
		home_job_orphan(job, bits);
		return;
	}
	struct home *home = self;
	if (bits & JOB_SUBMIT)
		home_submit(home, job, bits);
	else
		home_release(home, job);
}

/* Put @p home's timers in the heap from the start, so that setting them
 * never fails, and make its curl bridge, which counts into @p metrics. */
static int
home_arm (struct home         *home,
          struct http_metrics *metrics)
{
	struct loop_timer *const timers[] = {
		&home->deadline, &home->quiet, &home->breaker, &home->cancel_due,
	};
	for (size_t i = 0; i < ARRAY_SIZE(timers); ++i) {
		int e = loop_timer_set(home->loop, timers[i], UINT64_MAX);
		if (e)
			return e;
	}
	int e;
	home->http = http_create(home->loop, metrics, &e);
	return e;
}

/* Free what @p home holds, but for its jobs, however far home_create() got
 * in making it. */
static void
home_fini (struct home *home)
{
	http_cancel(home->http, &home->ask);
	while (!list_is_empty(&home->cancellations)) {
		struct home_cancellation *cancel = container_of(home->cancellations.next,
		                                               struct home_cancellation, hook);
		http_cancel(home->http, &cancel->transfer);
		list_del(&cancel->hook);
		free(cancel);
		cancel = nullptr;
	}
	struct loop_step *const steps[] = {
		&home->start, &home->stop, &home->drain, &home->overdue, &home->silent,
		&home->resume, &home->feed, &home->reap, &home->shed, &home->preempt,
		&home->props, &home->cleanup,
	};
	for (size_t i = 0; i < ARRAY_SIZE(steps); ++i)
		loop_unready(steps[i]);
	loop_timer_stop(home->loop, &home->breaker);
	loop_timer_stop(home->loop, &home->quiet);
	loop_timer_stop(home->loop, &home->deadline);
	loop_timer_stop(home->loop, &home->cancel_due);
	http_destroy(&home->http);
	jb_parser_fini(&home->parser, &home->pool, nullptr);
	jb_reaper_fini(&home->reaper);
	buf_fini(&home->answer);
	while (home_relieve(home, BUF_CRITICAL, SIZE_MAX))
		;
	self = nullptr;
	free(home->whom);
	home->whom = nullptr;
}

struct home *
home_create (struct loop              *loop,
             struct blocker           *blockers,
             struct httpd_home        *door,
             struct ai_endpoint const *ep,
             struct config const      *cfg,
             struct home_metrics      *metrics,
             struct http_metrics      *http,
             int                      *e)
{
	struct home *home = malloc(sizeof *home);
	if (!home) {
		*e = ENOMEM;
		return nullptr;
	}
	uint32_t max = cfg->calls;
	size_t buckets = stdc_bit_ceil(max < KEYS_MAX ? max : (uint32_t)KEYS_MAX);
	*home = (struct home){
		.start.fn = home_start_step,
		.stop.fn = home_stop_step,
		.drain.fn = home_drain_step,
		.overdue.fn = home_overdue_step,
		.silent.fn = home_silent_step,
		.resume.fn = home_resume_step,
		.feed.fn = home_feed_step,
		.reap.fn = home_reap_step,
		.shed.fn = home_shed_step,
		.preempt.fn = home_preempt_step,
		.props.fn = home_props_step,
		.cleanup.fn = home_cleanup_step,
		.deadline.fn = home_deadline_event,
		.quiet.fn = home_quiet_event,
		.breaker.fn = home_breaker_event,
		.cancel_due.fn = home_cleanup_event,
		.pool = {
			.scalar = MAX_SCALAR,
			.flags = JSON_TOKENER_VALIDATE_UTF8
		},
		.metrics = metrics,
		.loop = loop,
		.door = door,
		.ep = ep,
		.whom = calloc(buckets, sizeof *home->whom),
		.mask = buckets - 1,
		.timeout = ep->timeout_ms > 0 ? (uint64_t)ep->timeout_ms * 1'000'000 : 0,
		.silence = ep->chunk_ms > 0 ? (uint64_t)ep->chunk_ms * 1'000'000 : 0,
		.backoff = BACKOFF_MIN,
		.max = max,
		/* The context, which compacting takes, unless told, as /props
		 * says if asked. */
		.context = ep->context > 0 ? (uint32_t)ep->context : 0,
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
	list_init(&home->cancellations);
	list_init(&home->spare.list);
	*e = home->whom ? home_arm(home, http) : ENOMEM;
	if (*e) {
		home_fini(home);
		free(home);
		return nullptr;
	}
	http_resolver(home->http, blockers);
	self = home;
	/* Until /props answers, if asked, the default, said once it has. */
	if (!ep->slots) {
		home_slots(home, SLOTS, home_props_ask(home) ? "the default" : nullptr);
		return home;
	}
	/* Told the slots, /props takes no call's connection. */
	bool asked = cfg->flags & CFG_UPSTREAM_TOOLS && !home->context &&
	             !home_props_ask(home);
	home_slots(home, ep->slots, "--slots");
	if (asked)
		http_connections(home->http, ep->slots + 1L);
	return home;
}

void
home_destroy (struct home **p_home)
{
	if (!p_home || !*p_home)
		return;
	struct home *home = *p_home;
	*p_home = nullptr;
	while (!list_is_empty(&home->timeline))
		home_cancel(home, container_of(home->timeline.next, struct job, due));
	struct home_metrics const *m = home->metrics;
	struct {
		char const             *name;
		_Atomic uint64_t const *count;
	} const counts[] = {
		{ "submitted", &m->submitted },
		{ "completed", &m->completed },
		{ "cancelled", &m->cancelled },
		{ "rejected",  &m->rejected  },
		{ "aborted",   &m->aborted   },
		{ "preempted", &m->preempted },
	};
	LOG_ALL(LOG_INFO, "upstream requests:");
	for (size_t i = 0; i < ARRAY_SIZE(counts); ++i)
		LOG_ALL(LOG_INFO, "  %-9s  %" PRIu64, counts[i].name, hist_get(counts[i].count));
	home_fini(home);
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
	home_drain(home);
	home_maybe_exit(home);
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
		free(container_of(shelf_drop(&home->spare), struct home_principal, ring));
	return done;
}
