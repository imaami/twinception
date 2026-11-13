/** @file home.c
 *
 * Home's jobs: admission, the scheduler, the runs of the jobs over
 * the curl bridge, streamed answers and the breaker.
 */
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbit.h>
#include <stdlib.h>
#include <string.h>

#include "ai.h"
#include "buf.h"
#include "config.h"
#include "dstr.h"
#include "hexid.h"
#include "hist.h"
#include "home.h"
#include "htab.h"
#include "http.h"
#include "httpd.h"
#include "jb.h"
#include "job.h"
#include "journal.h"
#include "list.h"
#include "log.h"
#include "loop.h"
#include "ref.h"
#include "shelf.h"
#include "util.h"

enum {
	MAX_RESPONSE = 16 << 20,  /* the most bytes of an answer, of a streamed
	                             one's text, and of an event of it */
	MAX_PROPS    = 1 << 20,   /* bytes of a /props answer */
	PROPS_MS     = 10000,     /* the most /props may take, or the timeout */
	CANCEL_MS    = 5000,      /* native cancellation's total lifetime */
	MAX_JOB_ID   = 4096,      /* bytes of a native provider job's id */
	MAX_CANCEL   = 64 << 10,  /* bytes of the answer to a cancellation */
	MAX_ERROR    = 4096,      /* bytes of an event's error text that the
	                             result shows, at most */
	KEYS_MAX     = 1 << 16,   /* the largest number of buckets of the
	                             principals */
	SLOTS        = 4,         /* upstream calls at once, unless told or found */
	FAILURES     = 5,         /* failures in a row that open the breaker */
};

#define BACKOFF_MIN NS
#define BACKOFF_MAX (60 * NS)

/* What home notes of a job's run, in its flags. */
enum : uint32_t {
	OVERFLOW      = 1u << 0,   /* the answer passed MAX_RESPONSE */
	DROPPED       = 1u << 1,   /* its front let go of it while it ran */
	PROVIDER_DONE = 1u << 2,   /* the provider's native job is in a final
	                              state */
	RUNNING       = 1u << 3,   /* it uses one of the upstream slots */
	PROBE         = 1u << 4,   /* it is the breaker's probe */
	SEEN          = 1u << 5,   /* its answer has begun */
	STREAM        = 1u << 6,   /* the answer is an event stream */
	BACKED        = 1u << 7,   /* held back until its stream is read on */
	DELIVERED     = 1u << 8,   /* libcurl has handed over all of the stream */
	EVENT         = 1u << 9,   /* an event of it is whole, and is being
	                              parsed */
	TRUNCATED     = 1u << 10,  /* its stop reason is the output token limit */
	ERRED         = 1u << 11,  /* the result is the text of the upstream's
	                              error */
	STALLED       = 1u << 12,  /* the chunk timeout ended it */
	ENDED         = 1u << 13,  /* an event of its stream marked the end of
	                              the answer */
	READY         = 1u << 14,  /* a slot was free for it: on a ready list */
	PREEMPTED     = 1u << 15,  /* an interactive job preempted it once, which
	                              no other may again */
	DELTA_PENDING = 1u << 16,  /* the front has not yet drained the delta
	                              mailbox */
	DELTA_ENDED   = 1u << 17,  /* the job has ended, and DONE is posted once
	                              the front drains the mailbox */
};

/* The breaker's states, as /metrics gives them. */
enum { CLOSED, OPEN, HALF };

/* The timers of struct home, by their index in home->timers. */
enum {
	TIMER_DEADLINE,  /* at the deadline of the first job of the timeline */
	TIMER_QUIET,     /* when the answer silent longest has been silent too
	                    long */
	TIMER_BREAKER,   /* when the breaker half-opens */
	TIMER_CANCEL,    /* at the deadline of the first native cancellation */
	TIMERS
};

/* The bits of home->flags. */
enum : uint16_t {
	HOME_DRAINING = 1u << 0,  /* home cancels the jobs in flight and
	                             refuses new ones */
	HOME_ALONE    = 1u << 1,  /* the client loops have exited */
	HOME_PROBING  = 1u << 2,  /* the breaker's probe is under way */
	HOME_FIFO     = 1u << 3,  /* all jobs of a class wait in one queue */
};

/* Whom jobs of a class are for: an identity, a session, or a client's
 * address. An entry exists while the principal has jobs of the class
 * queued, which tell who it is, and is kept for reuse after.
 */
struct home_principal {
	struct htab_node node;    /* in home->whom, with the hash of the
	                             principal as its hash, or with HOME_FIFO
	                             the class */
	struct list      ring;    /* on home's ring of its class, or its shelf */
	struct list      jobs;    /* queued, oldest first, by their queue hooks */
	uint32_t         queued;
	uint32_t         class;   /* 0 interactive, 1 background */
};

/* Best-effort cancellation outlives the local job, but not its deadline. */
struct home_cancellation {
	struct list          hook;
	struct http_request *transfer;
	uint64_t             deadline;
	size_t               received;
};

/* Only failed native streams need this: buffered events can still carry
 * the id that lets us cancel the provider's generation.
 */
struct home_delivery_error {
	long retry;
	long result;                   /* a CURLcode, as a long so that the
	                                  struct has no padding */
	char message[CURL_ERROR_SIZE];
};

/* The bits of job->recovery.flags. */
enum : uint64_t {
	RECOVERY_RESTORING = 1u << 0,  /* the job was restored from its journal,
	                                  and home_start() reattaches to the
	                                  provider's stream */
	RECOVERY_CANCEL    = 1u << 1,  /* its front cancelled it with JOB_CANCEL */
	RECOVERY_DEFERRED  = 1u << 2,  /* its transfer ended during a journal
	                                  write */
	RECOVERY_OPENING   = 1u << 3,  /* its stream opens once the "meta"
	                                  record is written */
	RECOVERY_RESUME    = 1u << 4,  /* with RECOVERY_OPENING, the stream
	                                  resumes the provider's job */
	RECOVERY_ENDING    = 1u << 5,  /* it ended during a journal write */
	RECOVERY_THOUGHT   = 1u << 6,  /* a thought is open in the journal */
	RECOVERY_FINAL     = 1u << 7,  /* the final thought boundary is written */
};

/*
 * Every job in flight is on the timeline, in the order of its deadline: the
 * front's, from its submission, while it waits, and from its start upstream
 * once it runs, which moves it to the end. A job waiting is on its
 * principal's queue, and the principals with jobs queued are on the ring of
 * their jobs' class, the next to start one first: each starts one, then
 * goes to the back; or, if a slot was free for it as it came and nobody
 * waited, it is ready, to start before them. Interactive jobs start before
 * background ones, which hold all slots but one at most, so that one stays
 * free for interactive work; with one slot, an interactive job that has to
 * wait preempts a background one running, which starts again later, ready,
 * before the other background jobs. A running background job is among
 * those of its class, unless its front let go of it: then it is among
 * those dropped, to cancel. Messages only admit, refuse or move jobs, and bulk
 * steps start, read and cancel them, a share at a time, as each call into
 * libcurl costs some tens of µs. The principals with jobs queued are in a
 * hash table, by the seeded hash of each principal.
 */
struct home {
	struct loop_step          start;       /* starts the jobs waiting */
	struct loop_step          stop;        /* cancels the jobs on dropped, or
	                                          every job on timeline while
	                                          HOME_DRAINING is set */
	struct loop_step          overdue;     /* ends the jobs past their deadline */
	struct loop_step          silent;      /* ends the answers silent too long */
	struct loop_step          feed;        /* reads the streams */
	struct loop_step          reap;        /* frees the events' values */
	struct loop_step          shed;        /* ends the jobs waiting, the breaker
	                                          open */
	struct loop_step          preempt;     /* preempts the jobs preempting */
	struct loop_step          props;       /* reads /props */
	struct loop_step          cleanup;     /* ends overdue native cancellations */
	struct loop_timer         timers[TIMERS];
	struct list               ring[2];     /* by class */
	struct list               ready[2];    /* by class, by the jobs' queue
	                                          hooks */
	struct list               background;  /* the background jobs running, by
	                                          their queue hooks */
	struct list               preempting;  /* those to preempt, likewise */
	struct list               timeline;
	struct list               dropped;
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
	struct htab               whom;        /* the principals with jobs queued */
	struct http_request      *ask;         /* GET /props, under way */
	char const              **headers;     /* with llama, the upstream's
	                                          header lines without an
	                                          X-Conversation-Id line, and
	                                          then the line in conversation;
	                                          nullptr otherwise */
	uint64_t                  timeout;     /* ns from a start to its deadline */
	uint64_t                  silence;     /* ns an answer may pause */
	uint64_t                  backoff;     /* the breaker's next open spell */
	uint64_t                  until;       /* when the breaker half-opens */
	char                      conversation[64];  /* the X-Conversation-Id
	                                                line of the job that
	                                                home_open() starts */
	uint32_t                  max;         /* jobs in flight at most */
	uint32_t                  slots;       /* upstream calls at once at most */
	uint32_t                  context;     /* tokens of the upstream's context,
	                                          as told or found; 0: unknown */
	uint32_t                  failures;    /* in a row, closed */
	uint32_t                  readied[2];  /* jobs ready, by class */
	uint32_t                  running_bg;  /* background jobs running */
	uint32_t                  cancelling;  /* native cancellation requests */
	uint32_t                  persisting;  /* journal callbacks still using home */
	uint16_t                  flags;       /* HOME_* */
	uint16_t                  state;       /* the breaker's */
};

/* The thread's home: the handler of jobs' messages finds it here. */
static thread_local struct home *self;

static int home_open (struct home *home, struct job *job, bool resume);
static void home_archive_pending (struct home *home, struct job *job);
static int home_provider_url (struct home *home, struct job *job,
                              bool cancel, char **out);

/* The names that home uses for each API. llama is the end of the path of
 * the API's endpoint at llama-server, if its background mode serves the
 * API. type and object are the members of a native background event with
 * its type and its object, created is the type of the event that creates
 * the object. If update is not empty, it is the type of an event that
 * contains the status as a top-level member instead of in its object.
 */
static struct home_api {
	struct cstr_view  llama;
	struct cstr_view  created;
	struct cstr_view  update;
	char const       *type;
	char const       *object;
} const home_apis[] = {
	[AI_CHAT]         = { .llama = CSTR_VIEW_INIT("chat/completions") },
	[AI_RESPONSES]    = {
		.llama = CSTR_VIEW_INIT("responses"),
		.type = "type",
		.object = "response",
		.created = CSTR_VIEW_INIT("response.created")
	},
	[AI_MESSAGES]     = { .llama = CSTR_VIEW_INIT("messages") },
	[AI_INTERACTIONS] = {
		.type = "event_type",
		.object = "interaction",
		.created = CSTR_VIEW_INIT("interaction.created"),
		.update = CSTR_VIEW_INIT("interaction.status_update")
	},
};

/* Whether the upstream is llama-server in its background mode. */
static bool
home_llama (struct home const *home)
{
	return home->ep->background && home->ep->provider == AI_LLAMA &&
	       home_apis[home->ep->api].llama.len;
}

/* The field name of llama's conversation header line. */
static struct cstr_view const conversation_id = CSTR_VIEW_INIT("X-Conversation-Id:");

/* Whether the provider keeps a job's generation, which askai can reattach
 * to and must cancel: native background execution, or llama's.
 */
static bool
home_native (struct home const *home)
{
	return ai_background(home->ep) || home_llama(home);
}

void
home_job_recovery_fini (struct job *job)
{
	free(job->recovery.provider_id);
	job->recovery = (struct home_recovery){};
}

int
home_job_restore (struct job                  *job,
                  struct journal_record const *records)
{
	struct home_recovery *r = &job->recovery;
	if (r->flags)
		return EBUSY;
	r->flags = RECOVERY_RESTORING;
	for (; records; records = records->next) {
		if (records->type != JOURNAL_META)
			continue;
		struct json_object *meta = jb_parse((char const *)records->data,
		                                    records->len, 0, 8);
		char const *id = jb_view(jb_get(meta, "provider_id")).data;
		int e = meta ? 0 : EPROTO;
		if (!e && id && *id && !r->provider_id && !(r->provider_id = strdup(id)))
			e = ENOMEM;
		json_object_put(meta);
		if (e) {
			home_job_recovery_fini(job);
			return e;
		}
	}
	return 0;
}

/* @p job's class: 0 interactive, 1 background. */
static uint32_t
home_job_class (struct job const *job)
{
	return (job->asks & JOB_BACKGROUND) != 0;
}

/* The upstream slots that background jobs may hold at most: all but one,
 * which stays for interactive work, unless there is only one.
 */
static uint32_t
home_background_slots (struct home const *home)
{
	return home->slots - (home->slots >= 2);
}

/* Whether the job @p key is for the principal of @p node: the class and
 * the session of the job, and its key or its peer, equal those of the first
 * job in the principal's queue (htab_match_fn).
 */
static bool
home_principal_is (struct htab_node const *node,
                   void const             *key)
{
	struct home_principal const *p = container_of(node, struct home_principal, node);
	struct job const *job = key;
	struct job const *first = container_of(p->jobs.next, struct job, queue);
	return p->class == home_job_class(job) && first->session == job->session &&
	       (job->session ? !memcmp(buf_peek(&first->key).ptr,
	                               buf_peek(&job->key).ptr, job->session)
	                     : stream_peer_equal(first->peer, job->peer));
}

/* Whether the job @p key is of the class of the principal of @p node, as
 * with HOME_FIFO (htab_match_fn).
 */
static bool
home_class_is (struct htab_node const *node,
               void const             *key)
{
	return container_of(node, struct home_principal, node)->class == home_job_class(key);
}

/* The principal that @p job is for, or nullptr if no principal with jobs
 * queued matches the job. With HOME_FIFO, the principal of all jobs of a
 * class is the class.
 */
static struct home_principal *
home_find_whom (struct home const *home,
                struct job const  *job)
{
	struct htab_node *node = home->flags & HOME_FIFO
	                       ? *htab_find(&home->whom, home_job_class(job), home_class_is, job)
	                       : *htab_find(&home->whom, job->principal, home_principal_is, job);
	return node ? container_of(node, struct home_principal, node) : nullptr;
}

/* Publish @p job's progress, and tell its front if it asked. */
static void
home_job_progress (struct job *job,
                   uint64_t    progress)
{
	hist_set(&job->progress, progress);
	if (job->asks & JOB_NOTIFY && !(job->flags & DROPPED))
		job_post(job, job->front, &job->to_front, &job->front_bits, JOB_PROGRESS);
}

/* Take @p job off its principal's queue, the jobs behind it each a place
 * nearer, at most JOB_QUEUED - 1 (in one queue for all, which has no cap,
 * they keep the places they had); a principal with none left goes on the
 * shelf.
 */
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
		htab_unlink(&home->whom, &p->node);
		list_del(&p->ring);
		shelf_put(&home->spare, &p->ring);
		hist_add(&home->metrics->principals, -1ULL);
		return;
	}
	if (home->flags & HOME_FIFO)
		return;
	for (; node != &p->jobs; node = node->next) {
		struct job *next = container_of(node, struct job, queue);
		home_job_progress(next, hist_get(&next->progress) - 1);
	}
}

/* Give the storage of @p job's answer back to the stash. */
static void
home_job_drop_answer (struct job *job)
{
	job_drop_answer(job);
	job->flags &= ~(DELTA_PENDING | DELTA_ENDED);
}

/* Drop @p n references to @p job, and hand it back to its front if they were
 * the last.
 */
static void
home_job_put (struct job *job,
              uint32_t    n)
{
	if (ref_put(&job->refs, n)) {
		atomic_store_explicit(&job->front_bits, JOB_RETURN, memory_order_relaxed);
		loop_post(job->front, &job->to_front);
	}
}

/* Forget what home noted of @p job's last run, its flags now @p flags. */
static void
home_job_reset (struct job *job,
                uint32_t    flags)
{
	job->flags = flags;
	job->http = 0;
	job->ev = job->evlen = job->line = job->scan = job->chunks = job->used = 0;
	job->reader = (struct ai_reader){};
	job->reason[0] = '\0';
}

/* @p job has ended as @p status: its completion goes to its front. The
 * result of a job cancelled, which its front does not read, goes back to the
 * stash at once, and so does that of one its front let go of as it ran,
 * which home lets go of too.
 */
static void
home_job_end (struct job      *job,
              enum job_status  status)
{
	job->status = (uint8_t)status;
	if (job->recovery.pending) {
		job->recovery.flags |= RECOVERY_ENDING;
		return;
	}
	switch (job->flags & (DELTA_PENDING | DROPPED)) {
	case DELTA_PENDING:
		job->flags |= DELTA_ENDED;
		return;
	case 0:
		if (status == JOB_CANCELLED)
			home_job_drop_answer(job);
		job_post(job, job->front, &job->to_front, &job->front_bits, JOB_DONE);
		return;
	}
	/* DROPPED: the front reads neither the mailbox nor the result. */
	home_job_drop_answer(job);
	job_post(job, job->front, &job->to_front, &job->front_bits, JOB_DONE);
	home_job_put(job, 1);
}

/* Have the reap step free the events' values left, if any are. */
static void
home_reap (struct home *home)
{
	if (home->reaper.n)
		loop_ready_bulk(home->loop, &home->reap);
}

/* Let go of what @p job's answer under way holds: its places among the
 * answers silent and to read, its event's parser, whose values the reap
 * step frees, and its wire.
 */
static void
home_unstream (struct home *home,
               struct job  *job)
{
	list_del(&job->idle);
	list_del(&job->feed);
	jb_parser_fini(&job->parser, &home->pool, &home->reaper);
	home_reap(home);
	buf_fini(&job->wire);
	curl_free(job->cancel_url);
	job->cancel_url = nullptr;
	free(job->delivery_error);
	job->delivery_error = nullptr;
}

/* Takes @p job, which is in flight, off the timeline, off its principal's
 * queue and off its list of jobs. If the job runs upstream, the function
 * frees its upstream slot, so that the next job can start.
 */
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
		if (job->flags & PROBE)
			home->flags &= ~HOME_PROBING;
		loop_ready_bulk(home->loop, &home->start);
	}
	home_unstream(home, job);
}

/* On shutdown, leave time for the bounded native cancellations too. */
static void
home_maybe_exit (struct home *home)
{
	if (home->flags & HOME_ALONE && !hist_get(&home->metrics->in_flight) && !home->cancelling &&
	    !home->persisting)
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
	(void)loop_timer_set(home->loop, &home->timers[TIMER_CANCEL], due);
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
	if (len > MAX_CANCEL - cancel->received)
		return 0;
	cancel->received += len;
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
 * its stream alone would leave it running at the provider.
 */
static void
home_native_cancel (struct home *home,
                    struct job  *job)
{
	if (!home_native(home) || (job->flags & (RUNNING | PROVIDER_DONE)) != RUNNING)
		return;
	if (!job->cancel_url && job->recovery.provider_id)
		(void)home_provider_url(home, job, true, &job->cancel_url);
	char const *why = !job->cancel_url ? "no provider job id"
	                : home->cancelling >= home->max ? "cleanup queue full" : nullptr;
	struct home_cancellation *cancel = why ? nullptr : calloc(1, sizeof *cancel);
	if (!cancel) {
		LOG(LOG_WARN, "native background cancellation unavailable: %s",
		    why ? why : "out of memory");
		return;
	}
	cancel->deadline = home->loop->now + CANCEL_MS * MS;
	int e = http_post_json(home->http, job->cancel_url, "", 0, home->ep->headers,
	                       CANCEL_MS, home_cancel_write, home_cancel_done, cancel,
	                       &cancel->transfer);
	if (!e && home_llama(home))
		e = http_method(cancel->transfer, "DELETE");
	if (e) {
		http_cancel(home->http, &cancel->transfer);
		free(cancel);
		cancel = nullptr;
		LOG(LOG_WARN, "native background cancellation unavailable: %s", strerror(e));
		return;
	}
	list_append(&home->cancellations, &cancel->hook);
	++home->cancelling;
	if (home->timers[TIMER_CANCEL].due == UINT64_MAX)
		(void)loop_timer_set(home->loop, &home->timers[TIMER_CANCEL], cancel->deadline);
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
		if (loop_spent(loop, step, LOOP_REQS))
			return;
		http_cancel(home->http, &cancel->transfer);
		home_cancel_done(CURLE_OPERATION_TIMEDOUT, 0, "", cancel);
		loop_work(loop, LOOP_REQS, 1);
	}
}

static void
home_cleanup_event (struct loop       *loop,
                    struct loop_timer *timer)
{
	loop_ready_bulk(loop, &container_of(timer, struct home, timers[TIMER_CANCEL])->cleanup);
}

/* End @p job as @p status, counted in @p count, leaving the jobs in flight
 * if it is one (home_job_end()). Alone, home has its loop exit once no job
 * is in flight.
 */
static void
home_end (struct home      *home,
          struct job       *job,
          enum job_status   status,
          _Atomic uint64_t *count)
{
	if ((status == JOB_CANCELLED || status == JOB_FAILED) &&
	    (!(job->asks & JOB_DURABLE) || job->recovery.flags & RECOVERY_CANCEL))
		home_native_cancel(home, job);
	if (!list_is_empty(&job->due))
		home_retire(home, job);
	hist_add(count, 1);
	home_job_end(job, status);
	home_maybe_exit(home);
}

/* Put what @p fmt says, formatted, in @p job's result in its answer's place;
 * returns 0 or the error number that left it empty.
 */
[[gnu::format(printf, 2, 3)]]
static int
home_job_say (struct job *job,
              char const *fmt,
              ...)
{
	buf_clear(&job->result);
	va_list ap;
	va_start(ap, fmt);
	int e = buf_vprintf(&job->result, fmt, ap);
	va_end(ap);
	return e;
}

/* The upstream erred, as the @p len bytes at @p what say, all of them if
 * @p len is negative: say so in @p job's result (home_job_say()).
 */
static int
home_job_erred (struct job *job,
                char const *what,
                int         len)
{
	job->flags |= ERRED;
	return home_job_say(job, "Upstream error (HTTP %d): %.*s", job->http, len, what);
}

/* A journal write of @p job failed with the error in job->recovery.error:
 * say so in @p job's result (home_job_say()).
 */
static void
home_job_unpersisted (struct job *job)
{
	job->flags |= ERRED;
	(void)home_job_say(job, "Transcript persistence failed: %s", strerror(job->recovery.error));
}

/* The breaker is open: end @p job, which may not go upstream, with when to
 * try again.
 */
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
                  uint16_t     state)
{
	home->state = state;
	hist_set(&home->metrics->breaker, state);
}

/* Refuse the upstream jobs for a spell: what Retry-After asked for, @p retry
 * seconds, or else the backoff, which doubles for the next.
 */
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
	(void)loop_timer_set(loop, &home->timers[TIMER_BREAKER], home->until);
	LOG(LOG_WARN, "upstream unavailable: tool calls refused for %" PRIu64 " s",
	    spell / NS);
	loop_ready_bulk(loop, &home->shed);
}

#undef BACKOFF_MAX

static void
home_breaker_event (struct loop       *loop,
                    struct loop_timer *timer)
{
	struct home *home = container_of(timer, struct home, timers[TIMER_BREAKER]);
	home_breaker_set(home, HALF);
	LOG(LOG_INFO, "upstream breaker half-open: a probe goes next");
	loop_ready_bulk(loop, &home->start);
}

/* Whether @p job, a background job running, may be cut: not as the probe,
 * nor once its answer is all in. Native background work cannot be restarted
 * this way: closing its stream would leave the old generation running.
 * Archived thinking also belongs to one generation, not its replacement.
 */
static bool
home_job_cuttable (struct home const *home,
                   struct job const  *job)
{
	return !home_native(home) && !(job->asks & JOB_DURABLE) && job->transfer &&
	       !(job->recovery.pending || job->recovery.thought_offset) &&
	       !(job->flags & (PROBE | DELIVERED | ENDED));
}

/* How many background jobs running hold slots they may not: with one slot,
 * the one an interactive job waits for; else those beyond the S - 1 they
 * may hold, as S may shrink.
 */
static uint32_t
home_excess (struct home const *home)
{
	if (home->slots == 1 && home->running_bg && !list_is_empty(&home->ring[0]))
		return 1;
	uint32_t may = home_background_slots(home);
	return home->running_bg > may ? home->running_bg - may : 0;
}

/* Have the background jobs that hold slots they may not, but for those being
 * preempted already, preempted, the first to have started first.
 */
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
		list_move(&home->preempting, &job->queue);
		loop_ready_bulk(home->loop, &home->preempt);
		--over;
	}
}

/* The probe, @p job, has gone through, which closes the breaker, unless
 * @p failure, which opens it again, for @p retry seconds or the backoff.
 */
static void
home_breaker_probed (struct home *home,
                     struct job  *job,
                     bool         failure,
                     long         retry)
{
	job->flags &= ~PROBE;
	home->flags &= ~HOME_PROBING;
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
 * probe's success, which its answer's beginning tells, closes it. Other
 * jobs' outcomes count only while it is closed, and a job cancelled counts
 * not at all.
 */
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
 * ran out of, or an answer whose status is a server error or 429.
 */
static bool
home_job_failure (struct job const *job,
                  CURLcode          result)
{
	if (result)
		return result != CURLE_WRITE_ERROR && result != CURLE_OUT_OF_MEMORY;
	return job->http >= 500 || job->http == 429;
}

/* Say in @p job's result why its run upstream, ended with @p result and
 * libcurl's @p error, failed, unless an error event of its stream has.
 */
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
 * so, while the front tells tool calls cut short.
 */
static void
home_finish_text (struct home *home,
                  struct job  *job)
{
	_Atomic uint64_t *completed = &home->metrics->completed;
	if (buf_peek(&job->calls[0]).len) {
		home_end(home, job, JOB_TEXT, completed);
		return;
	}
	struct span s = {};
	if (!buf_peek(&job->result).len) {
		char const *reason = *job->reason ? job->reason : nullptr;
		if (!buf_reserve(&job->result, AI_NO_ANSWER, SIZE_MAX, &s))
			buf_commit(&job->result, ai_no_answer((char *)s.ptr, s.len, reason));
		home_end(home, job, JOB_FAILED, completed);
		return;
	}
	if (job->flags & TRUNCATED)
		(void)buf_put(&job->result, AI_CUT_NOTE, sizeof AI_CUT_NOTE - 1, SIZE_MAX);
	home_end(home, job, JOB_TEXT, completed);
}

/* @p job's run upstream is over, with @p result and libcurl's @p error, and
 * @p retry the seconds its answer's Retry-After asked for: complete it with
 * the answer, its text, or why there is none in its place.
 */
static void
home_finish (struct home *home,
             struct job  *job,
             CURLcode     result,
             char const  *error,
             long         retry)
{
	bool const failed = home_job_failure(job, result);
	home_breaker_note(home, job, failed, retry);
	if (result || job->flags & ERRED) {
		(void)home_explain(home, job, result, error);
		bool resumable = result && failed && !(job->flags & (ERRED | PROVIDER_DONE)) &&
		                 job->asks & JOB_DURABLE && job->journal && job->recovery.provider_id &&
		                 home_native(home) &&
		                 (!job->deadline || home->loop->now < job->deadline);
		home_end(home, job, resumable ? JOB_SUSPENDED : JOB_FAILED,
		         &home->metrics->completed);
		return;
	}
	if (job->flags & STREAM)
		home_finish_text(home, job);
	else
		home_end(home, job, JOB_ANSWERED, &home->metrics->completed);
}

/* End @p job's run upstream now, with @p result, cancelling its transfer if
 * it still has one: the upstream sees a hang-up.
 */
static void
home_cut (struct home *home,
          struct job  *job,
          CURLcode     result)
{
	http_cancel(home->http, &job->transfer);
	home_finish(home, job, result, "", 0);
}

/* Whether @p job's stream waits for its front to drain the delta mailbox,
 * or for a journal write.
 */
static bool
home_job_waits (struct job const *job)
{
	return job->flags & DELTA_PENDING || job->recovery.pending;
}

/* Have the feed step read @p job's stream. */
static void
home_feed (struct home *home,
           struct job  *job)
{
	if (home_job_waits(job))
		return;
	if (list_is_empty(&job->feed))
		list_append(&home->feeding, &job->feed);
	loop_ready_bulk(home->loop, &home->feed);
}

/* @p job's answer begins: note its status, and whether it is an event
 * stream. The upstream answers: the other jobs need not wait for the
 * probe's end, which may be minutes away.
 */
static void
home_head (struct home *home,
           struct job  *job)
{
	long status;
	char const *type;
	http_response(job->transfer, &status, &type);
	struct cstr_view media = cstr_view(type);
	(void)cstr_view_cut(&media, ';', &media);
	bool const stream = status >= 200 && status < 300 &&
	                    cstr_view_equal_case(cstr_view_trim(media),
	                                         CSTR_VIEW("text/event-stream"));
	job->flags = (job->flags & ~STREAM) | SEEN | (stream ? STREAM : 0);
	job->http = (uint16_t)status;
	if (job->flags & PROBE && !home_job_failure(job, CURLE_OK))
		home_breaker_probed(home, job, false, 0);
}

/* Take the @p len bytes at @p data of @p job's answer into @p to, up to
 * MAX_RESPONSE bytes. Returns as a write callback does.
 */
static size_t
home_receive (struct job *job,
              struct buf *to,
              char const *data,
              size_t      len)
{
	int e = buf_put(to, data, len, MAX_RESPONSE);
	if (e) {
		job->flags |= e == ENOBUFS ? OVERFLOW : 0;
		return 0;
	}
	return len;
}

/* Gather the upstream's answer, a stream into the wire and a plain one into
 * the result (home_receive()). A stream is held back too while a step's
 * share of it waits to be read. Each write counts as the answer's sign of
 * life.
 */
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
	list_move(&home->idle, &job->idle);
	if (home->silence && home->timers[TIMER_QUIET].due == UINT64_MAX)
		(void)loop_timer_set(home->loop, &home->timers[TIMER_QUIET],
		                     job->active + home->silence);
	if (!(job->flags & STREAM))
		return home_receive(job, &job->result, data, len);
	if (job->flags & DELTA_PENDING || buf_peek(&job->wire).len - job->scan >= LOOP_STEP_RECV) {
		job->flags |= BACKED;
		list_del(&job->idle);
		return CURL_WRITEFUNC_PAUSE;
	}
	size_t n = home_receive(job, &job->wire, data, len);
	if (n == len)
		home_feed(home, job);
	return n;
}

/* The upstream call has ended. A stream's end, once all of it is read; any
 * other answer, or none, at once.
 */
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
	/* An archive write under way, or the events still to read of a
	 * native background stream, wait for the transfer's end, which the
	 * job keeps. */
	bool pending = job->recovery.pending;
	bool events = result && job->flags & STREAM && ai_background(home->ep) &&
	              buf_peek(&job->wire).len;
	struct home_delivery_error *d = pending || events ? malloc(sizeof *d) : nullptr;
	if (d) {
		*d = (struct home_delivery_error){
			.retry = http_retry_after(home->http),
			.result = result
		};
		util_format(d->message, sizeof d->message, "%s", error);
		job->delivery_error = d;
		job->flags |= DELIVERED;
	}
	if (pending) {
		if (!d)
			job->recovery.error = ENOMEM;
		job->recovery.flags |= RECOVERY_DEFERRED;
		return;
	}
	if (d || (result == CURLE_OK && job->flags & STREAM)) {
		job->flags |= DELIVERED;
		home_feed(home, job);
		return;
	}
	home_finish(home, job, result, error, http_retry_after(home->http));
}

/* Finish an append, reasoning commit or recovery checkpoint. DONE cannot
 * expose the job for reuse until all archive callbacks have returned.
 */
static void
home_journal_done (struct loop *loop,
                   void        *arg,
                   int          error)
{
	struct home *home = self;
	struct job *job = arg;
	struct home_recovery *r = &job->recovery;
	--r->pending;
	if (home)
		--home->persisting;
	if (!home || loop->flags & LOOP_EXIT) {
		home_job_put(job, 1);
		return;
	}
	if (error)
		r->error = error;
	if (r->pending)
		goto done;
	if (r->flags & RECOVERY_ENDING) {
		r->flags &= ~RECOVERY_ENDING;
		if (r->error && job->status != JOB_CANCELLED) {
			home_job_unpersisted(job);
			job->status = JOB_FAILED;
		}
		home_job_end(job, job->status);
		goto done;
	}
	if (list_is_empty(&job->due))
		goto done;
	if (r->error) {
		home_job_unpersisted(job);
		home_cut(home, job, CURLE_OK);
		goto done;
	}
	if (r->flags & RECOVERY_OPENING) {
		bool resume = r->flags & RECOVERY_RESUME;
		r->flags &= ~(RECOVERY_OPENING | RECOVERY_RESUME);
		int e = home_open(home, job, resume);
		if (e) {
			(void)home_job_say(job, "Cannot %s provider stream: %s",
			                   resume ? "resume" : "open", strerror(e));
			home_end(home, job, JOB_FAILED, &home->metrics->completed);
		}
		goto done;
	}
	bool const deferred = r->flags & RECOVERY_DEFERRED;
	r->flags &= ~RECOVERY_DEFERRED;
	if (job->flags & STREAM) {
		home_feed(home, job);
	} else if (deferred) {
		struct home_delivery_error const *d = job->delivery_error;
		home_finish(home, job, d->result, d->message, d->retry);
	}
done:
	home_job_put(job, 1);
	home_maybe_exit(home);
}

/* Write the "meta" record of @p job, the id of the provider's job, to
 * job->journal, unless that is nullptr. home_journal_done() goes on once
 * the record is written. home_job_restore() reads the record.
 */
static int
home_metadata (struct home *home,
               struct job  *job)
{
	if (!job->journal)
		return 0;
	char const *id = job->recovery.provider_id;
	struct json_object *meta = jb_object("provider_id", json_object_new_string(id ? id : ""),
	                                     nullptr);
	if (!meta)
		return ENOMEM;
	struct cstr_view const json = jb_wire(meta);
	int e = json.data ? journal_put(job->journal, &job->archive, "meta", json.data, json.len,
	                                home->loop, home_journal_done, job)
	                  : ENOMEM;
	json_object_put(meta);
	if (!e)
		home_archive_pending(home, job);
	return e;
}

/* Keep the provider identity before a connection starts. Restart recovery
 * requests its stream from the beginning and verifies saved thought bytes.
 */
static int
home_connect (struct home *home,
              struct job  *job,
              bool         resume)
{
	if (!job->journal)
		return home_open(home, job, resume);
	if (resume) {
		int e = journal_thought(job->journal, &job->archive, job->archive_turn,
		                        0, nullptr, 0, home->loop, home_journal_done, job);
		if (e)
			return e;
		home_archive_pending(home, job);
	}
	job->recovery.flags |= RECOVERY_OPENING | (resume ? RECOVERY_RESUME : 0);
	return home_metadata(home, job);
}

static int
home_open (struct home *home,
           struct job  *job,
           bool         resume)
{
	struct ai_endpoint const *ep = home->ep;
	char const *const *headers = ep->headers;
	char *url = nullptr;
	struct span body = {};
	if (resume) {
		int e = home_provider_url(home, job, false, &url);
		if (e)
			return e;
	} else {
		body = buf_peek(&job->request);
		if (home->headers) {
			util_format(home->conversation, sizeof home->conversation, "%s %s",
			            conversation_id.data, job->recovery.provider_id);
			headers = home->headers;
		}
	}
	job->flags &= ~(SEEN | DELIVERED);
	int e = http_post_json(home->http, url ? url : ep->url, (char const *)body.ptr, body.len,
	                       headers, 0, home_job_write, home_job_done, job, &job->transfer);
	curl_free(url);
	return e;
}

/* When @p job is due: its deadline, or never (UINT64_MAX) without one. */
static uint64_t
home_due (struct job const *job)
{
	return job->deadline ? job->deadline : UINT64_MAX;
}

/* Order by deadline: restored jobs retain their original remaining time,
 * which can be shorter than a newly started job's.
 */
static void
home_timeline (struct home *home,
               struct job  *job)
{
	list_del(&job->due);
	uint64_t const due = home_due(job);
	struct list *before = &home->timeline;
	for (struct list *p = home->timeline.prev; p != &home->timeline; p = p->prev) {
		if (home_due(container_of(p, struct job, due)) <= due)
			break;
		before = p;
	}
	list_append(before, &job->due);
	(void)loop_timer_set(home->loop, &home->timers[TIMER_DEADLINE],
	                     home_due(container_of(home->timeline.next, struct job, due)));
}

/* Start or replay @p job upstream; half-open, as the breaker's probe. */
static void
home_start (struct home *home,
            struct job  *job)
{
	struct home_metrics *m = home->metrics;
	struct loop *loop = home->loop;
	if (home->state == HALF) {
		job->flags |= PROBE;
		home->flags |= HOME_PROBING;
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
	if (!(job->asks & JOB_DURABLE))
		job->deadline = home->timeout ? loop->now + home->timeout : 0;
	home_timeline(home, job);
	home_job_progress(job, 0);
	/* A restored job reattaches to its provider stream; it never submits
	 * the original request again. */
	struct home_recovery *r = &job->recovery;
	bool resume = r->flags & RECOVERY_RESTORING;
	int e = 0;
	if (resume) {
		r->flags &= ~RECOVERY_RESTORING;
	} else if (home_llama(home)) {
		struct hexid id;
		e = hexid_make(&id);
		if (!e && !(r->provider_id = strdup(id.hex)))
			e = ENOMEM;
	}
	if (!e)
		e = home_connect(home, job, resume);
	if (e) {
		(void)home_job_say(job, resume ? "Cannot reattach provider stream: %s. The "
		                                 "original request will not be submitted again."
		                               : "Cannot start provider stream: %s", strerror(e));
		home_end(home, job, JOB_FAILED, &m->completed);
	}
}

/* The job of class @p c to start next: the first ready, else the oldest of
 * the ring's first principal; nullptr if none waits.
 */
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
 * nullptr if none waits.
 */
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
 * the next principal on the ring starts its first and goes to the back.
 */
static void
home_start_step (struct loop      *loop,
                 struct loop_step *step)
{
	struct home *home = container_of(step, struct home, start);
	while (!(home->flags & (HOME_DRAINING | HOME_PROBING)) && home->state != OPEN &&
	       hist_get(&home->metrics->running) < home->slots) {
		struct job *job = home_next(home, true);
		if (!job)
			return;
		if (loop_spent(loop, step, LOOP_REQS))
			return;
		if (job->flags & READY) {
			job->flags &= ~READY;
			--home->readied[home_job_class(job)];
			list_del(&job->queue);
		} else {
			list_move(&home->ring[home_job_class(job)], &job->who->ring);
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
	loop_ready_bulk(loop, &container_of(timer, struct home, timers[TIMER_DEADLINE])->overdue);
}

/* Cancel @p job's transfer, if it has one: one whose request had gone
 * upstream counts as cancelled there.
 */
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
 * slot does too, refused, and one dropped is cancelled. libcurl short of
 * memory may lose a transfer's own timeout.
 */
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
 * of requests allows (home_expire()); then wait for the next one's.
 */
static void
home_overdue_step (struct loop      *loop,
                   struct loop_step *step)
{
	struct home *home = container_of(step, struct home, overdue);
	for (; !list_is_empty(&home->timeline); loop_work(loop, LOOP_REQS, 1)) {
		struct job *job = container_of(home->timeline.next, struct job, due);
		uint64_t const due = home_due(job);
		if (due > loop->now) {
			(void)loop_timer_set(loop, &home->timers[TIMER_DEADLINE], due);
			return;
		}
		if (loop_spent(loop, step, LOOP_REQS))
			return;
		home_expire(home, job);
	}
	(void)loop_timer_set(loop, &home->timers[TIMER_DEADLINE], UINT64_MAX);
}

static void
home_quiet_event (struct loop       *loop,
                  struct loop_timer *timer)
{
	loop_ready_bulk(loop, &container_of(timer, struct home, timers[TIMER_QUIET])->silent);
}

/* End the answers that sent nothing for the chunk timeout, the one silent
 * longest first, as many a step as its share of requests allows; then wait
 * for the next.
 */
static void
home_silent_step (struct loop      *loop,
                  struct loop_step *step)
{
	struct home *home = container_of(step, struct home, silent);
	for (; !list_is_empty(&home->idle); loop_work(loop, LOOP_REQS, 1)) {
		struct job *job = container_of(home->idle.next, struct job, idle);
		uint64_t due = job->active + home->silence;
		if (due > loop->now) {
			(void)loop_timer_set(loop, &home->timers[TIMER_QUIET], due);
			return;
		}
		if (loop_spent(loop, step, LOOP_REQS))
			return;
		job->flags |= STALLED;
		hist_add(&home->metrics->stalls, 1);
		home_cut(home, job, CURLE_OPERATION_TIMEDOUT);
	}
	(void)loop_timer_set(loop, &home->timers[TIMER_QUIET], UINT64_MAX);
}

/* Append @p text to @p to, the answer, the thinking or the replay of a
 * job, if @p text is UTF-8 and the buffer stays within MAX_RESPONSE.
 * json-c lets overlong forms through, so this function checks the
 * encoding again. The text is appended whole or not at all.
 */
static int
home_append_text (struct home      *home,
                  struct buf       *to,
                  struct cstr_view  text)
{
	if (jb_utf8(text.data, text.len) < text.len)
		return EILSEQ;
	if (!text.len)
		return 0;
	int e = buf_put(to, text.data, text.len, MAX_RESPONSE);
	if (e)
		return e;
	loop_work(home->loop, LOOP_COPY, text.len);
	return 0;
}

/* Append @p text after a struct job_delta header to job->delta, the
 * mailbox from which the client loop streams the answer, if job->asks
 * contains JOB_STREAM. Parsing stops after the mailbox is published, so
 * the mailbox contains the pieces of at most one event while the client
 * loop is slow. The size of an event from the upstream is limited too.
 */
static int
home_put_delta (struct job       *job,
                struct cstr_view  text,
                bool              thought)
{
	if (!(job->asks & JOB_STREAM) || job->flags & DROPPED || !text.len)
		return 0;
	if (text.len > MAX_RESPONSE)
		return ENOBUFS;
	struct job_delta header = { .len = (uint32_t)text.len, .thought = thought };
	size_t len = sizeof header + text.len;
	struct span s = {};
	int e = buf_reserve(&job->delta, len, 2 * MAX_RESPONSE, &s);
	if (e)
		return e;
	memcpy(s.ptr, &header, sizeof header);
	memcpy(s.ptr + sizeof header, text.data, text.len);
	buf_commit(&job->delta, len);
	loop_work(self->loop, LOOP_COPY, len);
	return 0;
}

static void
home_archive_pending (struct home *home,
                      struct job  *job)
{
	++job->recovery.pending;
	++home->persisting;
	ref_get(&job->refs);
}

/* A boundary commits the concatenated text once. A later reasoning block
 * in the same provider turn may append again and have its own boundary.
 * The final boundary also verifies replay with no reasoning against any
 * saved prefix; a provider cannot silently replace it with an empty turn.
 */
static int
home_thought_end (struct home *home,
                  struct job  *job,
                  bool         final)
{
	struct home_recovery *r = &job->recovery;
	uint64_t const thought = r->flags & (RECOVERY_THOUGHT | RECOVERY_FINAL);
	if (!job->journal || thought & RECOVERY_FINAL || (!thought && !final))
		return 0;
	int e = journal_thought_end(job->journal, &job->archive, job->archive_turn,
	                            final, home->loop, home_journal_done, job);
	if (e) {
		r->error = e;
		return e;
	}
	r->flags = (r->flags & ~RECOVERY_THOUGHT) | (final ? RECOVERY_FINAL : 0);
	home_archive_pending(home, job);
	return 0;
}

/* Append @p text, a piece of the text of the answer, to the result of the
 * job @p user, and then to the mailbox of the job (home_put_delta()). Text
 * that is not empty first ends the open thought in the journal.
 */
static int
home_put_text (void             *user,
               struct cstr_view  text)
{
	struct job *job = user;
	int e = text.len ? home_thought_end(self, job, false) : 0;
	e = e ? e : home_append_text(self, &job->result, text);
	return e ? e : home_put_delta(job, text, false);
}

/* Write @p text, a piece of the thinking of @p job, to the journal of the
 * job after the thinking that is already written.
 */
static int
home_journal_thought (struct home      *home,
                      struct job       *job,
                      struct cstr_view  text)
{
	struct home_recovery *r = &job->recovery;
	int e = text.len > UINT64_MAX - r->thought_offset ? EOVERFLOW :
	        journal_thought(job->journal, &job->archive, job->archive_turn,
	                        r->thought_offset, text.data, text.len, home->loop,
	                        home_journal_done, job);
	if (e) {
		r->error = e;
		return e;
	}
	r->thought_offset += text.len;
	r->flags |= RECOVERY_THOUGHT;
	home_archive_pending(home, job);
	return 0;
}

/* Pass @p text, a piece of the thinking of the model, to the job @p user.
 * For a tool job of chat completions, the text is appended to
 * job->thought, because askai sends the thinking back to chat completions
 * in the member reasoning_content, and to the other APIs in the replay.
 * The text also goes to the journal of the job, if job->journal is not
 * nullptr, and to the mailbox of the job.
 */
static int
home_put_thought (void             *user,
                  struct cstr_view  text)
{
	struct job *job = user;
	int e = job->asks & JOB_TOOLS && self->ep->api == AI_CHAT
	      ? home_append_text(self, &job->thought, text)
	      : jb_utf8(text.data, text.len) < text.len ? EILSEQ : 0;
	if (!e && text.len && job->journal)
		e = home_journal_thought(self, job, text);
	return e ? e : home_put_delta(job, text, true);
}

/* Append @p json, a piece of the JSON text that askai sends back with the
 * turn, to the replay of the job @p user.
 */
static int
home_put_replay (void             *user,
                 struct cstr_view  json)
{
	struct job *job = user;
	return home_append_text(self, &job->replay, json);
}

/* Add the piece @p call of a tool call to the calls of the job @p user.
 * Call k is stored in job->calls[k], and the calls are numbered in the
 * order in which they begin. A piece with an index that differs from the
 * index of every stored call begins a new call. A piece without an index
 * begins a new call if there is no call yet or if its id differs from the
 * id of the last call. Otherwise the piece belongs to the last call. The
 * pieces of different calls may come in any order. In each call, the id
 * comes before the name, and the name comes before the extra. The extra
 * comes once and before the arguments. The arguments may come in several
 * pieces. The function ignores an id, a name or an extra that repeats the
 * stored bytes, because some servers send them again. The function
 * returns EPROTO if a call breaks this order or if there are more than
 * JOB_CALLS calls. It returns ENOBUFS if the calls together would exceed
 * MAX_RESPONSE bytes, and EILSEQ if a piece is not UTF-8.
 */
static int
home_put_call (void                 *user,
               struct ai_call const *call)
{
	struct job *job = user;
	int end = home_thought_end(self, job, false);
	if (end || !(job->asks & JOB_TOOLS))
		return end;
	/* This loop counts the calls in n and the bytes of all calls in bytes.
	 * It sets k to the number of the call with the index of the piece, c
	 * to the header of the last call, and last to the parts of the last
	 * call. most is the number of buffers, and k stays most if no stored
	 * call is at the index of the piece. */
	struct job_call c = {};
	struct ai_call last = ai_call();
	uint32_t const most = array_size(job->calls);
	uint32_t n = 0;
	uint32_t k = most;
	size_t bytes = 0;
	for (; n < most; ++n) {
		struct span s = buf_peek(&job->calls[n]);
		if (!s.len)
			break;
		last = job_call_parts(&c, s.ptr);
		k = c.index == call->index ? n : k;
		bytes += s.len;
	}
	size_t id = call->id.len;
	size_t name = call->name.len;
	size_t extra = call->extra.len;
	size_t args = call->args.len;
	uint32_t index = call->index;
	/* A piece without an index continues the last call if there is a
	 * call and the id of the piece is empty or equal to the id of the last
	 * call. Then its id is not stored again. Otherwise the piece begins
	 * call n. The index of a stored call is never AI_CALL_NEXT, so k is
	 * still most for such a piece. */
	if (index == AI_CALL_NEXT) {
		index = n;
		if (n && (!id || cstr_view_equal(last.id, call->id))) {
			k = n - 1;
			id = 0;
		}
	}
	size_t head = 0;
	if (k < n) {
		/* Ignore an id, a name or an extra that repeats the stored
		 * bytes. */
		struct ai_call const stored = k + 1 == n ? last
		                            : job_call_parts(&c, buf_peek(&job->calls[k]).ptr);
		if (cstr_view_equal(stored.id, call->id))
			id = 0;
		if (cstr_view_equal(stored.name, call->name))
			name = 0;
		if (cstr_view_equal(stored.extra, call->extra))
			extra = 0;
	} else if (n == most) {
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
	if (jb_utf8(call->id.data, id) < id || jb_utf8(call->name.data, name) < name ||
	    jb_utf8(call->extra.data, extra) < extra || jb_utf8(call->args.data, args) < args)
		return EILSEQ;
	size_t len = head + id + name + extra + args;
	if (!len)
		return 0;
	/* The limit of the buffer is MAX_RESPONSE minus the bytes of the
	 * other calls, so the calls together stay within MAX_RESPONSE. */
	struct buf *to = &job->calls[k];
	struct span s = {};
	int e = buf_reserve(to, len, MAX_RESPONSE - (bytes - buf_peek(to).len), &s);
	if (e)
		return e;
	/* After the order check above, the new bytes belong to the last
	 * stored part of the call or to the parts after it, so they go to the
	 * end of the buffer. */
	unsigned char *p = s.ptr + head;
	if (id)
		memcpy(p, call->id.data, id);
	if (name)
		memcpy(p + id, call->name.data, name);
	if (extra)
		memcpy(p + id + name, call->extra.data, extra);
	if (args)
		memcpy(p + id + name + extra, call->args.data, args);
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
 * the step's shares allow, which it counts; true once the parse is over.
 */
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
	bool over = jb_parser_step(parser, &home->pool, json, len, bytes,
	                           (uint32_t)values) != EAGAIN;
	loop_work(loop, LOOP_PARSE, parser->off - off);
	loop_work(loop, LOOP_VALUES, parser->cut.values - seen);
	loop_work(loop, LOOP_COPY, parser->cut.copied - copied);
	return over;
}

/* The provider can supply a path component, never a host or a replacement
 * query. Current authentication and configured query survive.
 */
static int
home_provider_url (struct home *home,
                   struct job  *job,
                   bool         cancel,
                   char       **out)
{
	char const *id = job->recovery.provider_id;
	if (!id || !*id)
		return ENODATA;
	if (!home_native(home))
		return ENOTSUP;
	CURLU *url = curl_url();
	char *escaped = curl_easy_escape(nullptr, id, 0);
	char *path = nullptr;
	dstr_auto tail = DSTR_INIT;
	dstr_auto query = DSTR_INIT;
	int e = ENOMEM;
	if (!url || !escaped)
		goto done;
	e = EINVAL;
	if (curl_url_set(url, CURLUPART_URL, home->ep->url, 0) ||
	    curl_url_get(url, CURLUPART_PATH, &path, 0))
		goto done;
	struct cstr_view base = cstr_view(path);
	while (base.len && base.data[base.len - 1] == '/')
		--base.len;
	bool ok;
	if (home_llama(home)) {
		struct cstr_view const suffix = home_apis[home->ep->api].llama;
		if (!cstr_view_ends(base, suffix))
			goto done;
		ok = dstr_cat_n(&tail, base.data, base.len - suffix.len) && dstr_cat(&tail, "stream") &&
		     dstr_printf(&query, "conv_id=%s&from=0", escaped);
	} else {
		ok = dstr_cat_n(&tail, base.data, base.len) &&
		     dstr_printf(&tail, "/%s%s", escaped, cancel ? "/cancel" : "") &&
		     (cancel || dstr_cat(&query, "stream=true"));
	}
	e = ENOMEM;
	if (!ok)
		goto done;
	e = EINVAL;
	if (curl_url_set(url, CURLUPART_PATH, dstr_get(&tail), 0) ||
	    (dstr_len(&query) &&
	     curl_url_set(url, CURLUPART_QUERY, dstr_get(&query), CURLU_APPENDQUERY)) ||
	    curl_url_get(url, CURLUPART_URL, out, 0))
		goto done;
	e = 0;
done:
	curl_free(path);
	curl_free(escaped);
	curl_url_cleanup(url);
	return e;
}

/* Retain the creation id while its event still owns the string. */
static int
home_native_id (struct home        *home,
                struct job         *job,
                struct json_object *value)
{
	struct cstr_view const id = jb_text(value, 1, MAX_JOB_ID, nullptr);
	if (!id.data || cstr_view_equal(id, CSTR_VIEW(".")) || cstr_view_equal(id, CSTR_VIEW("..")))
		return EPROTO;
	char *copy = strndup(id.data, id.len);
	if (!copy)
		return ENOMEM;
	free(job->recovery.provider_id);
	job->recovery.provider_id = copy;
	return home_provider_url(home, job, true, &job->cancel_url);
}

/* Capture the native creation id while its event is alive, and remember
 * terminal provider states so cleanup does not cancel completed work.
 */
static int
home_native_event (struct home        *home,
                   struct job         *job,
                   struct json_object *root)
{
	if (!ai_background(home->ep))
		return 0;
	static struct cstr_view const ends[] = {
		CSTR_VIEW_INIT("completed"), CSTR_VIEW_INIT("failed"),
		CSTR_VIEW_INIT("cancelled"), CSTR_VIEW_INIT("incomplete"),
	};
	struct home_api const *api = &home_apis[home->ep->api];
	struct cstr_view const kind = jb_view(jb_get(root, api->type));
	struct json_object *object = jb_get(root, api->object);
	struct json_object *status = jb_get(api->update.len && cstr_view_equal(kind, api->update)
	                                    ? root : object, "status");
	if (cstr_view_index(ends, array_size(ends), jb_view(status)) < array_size(ends))
		job->flags |= PROVIDER_DONE;
	if (job->cancel_url || !cstr_view_equal(kind, api->created))
		return 0;
	int e = home_native_id(home, job, jb_get(object, "id"));
	return e ? e : home_metadata(home, job);
}

/* Take what @p root, an event of @p job's stream, says (ai_event()): its
 * text, and with JOB_TOOLS its thinking and tool calls; its usage, its end,
 * and its stop reason, which may be the output token limit. Each event is
 * a chunk, and one with text or thinking a token. An error it says, or one
 * that taking it meets of the answer, goes in the result. Returns the
 * error that taking it met: one past MAX_RESPONSE is an overflow.
 */
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
	struct ai_sink const sink = ai_sink(
		.text    = home_put_text,
		.thought = job->asks & (JOB_TOOLS | JOB_STREAM) || job->journal
		         ? home_put_thought : nullptr,
		.call    = home_put_call,
		.replay  = job->asks & JOB_TOOLS ? home_put_replay : nullptr,
		.user    = job
	);
	size_t had = buf_peek(&job->result).len;
	struct ai_event ev = {};
	e = ai_event(home->ep->api, &job->reader, root, &sink, &ev);
	if (!e && ev.flags & (AI_EV_THOUGHT_END | AI_EV_END))
		e = home_thought_end(home, job, ev.flags & AI_EV_END);
	hist_add(&home->metrics->chunks, 1);
	if (buf_peek(&job->result).len > had)
		home_job_progress(job, (uint64_t)++job->chunks << 32);
	else if (ev.flags & AI_EV_THOUGHT)
		home_job_progress(job, (uint64_t)++job->chunks << 32 | JOB_THINKING);
	job->used = ev.used ? ev.used : job->used;
	job->flags |= (ev.flags & AI_EV_END ? ENDED | PROVIDER_DONE : 0) |
	              (e == ENOBUFS ? OVERFLOW : 0);
	if (ev.reason) {
		size_t len = jb_utf8(ev.reason, strnlen(ev.reason, sizeof job->reason - 1));
		memcpy(job->reason, ev.reason, len);
		job->reason[len] = '\0';
		job->flags = ev.flags & AI_EV_TRUNCATED ? job->flags | TRUNCATED
		                                         : job->flags & ~TRUNCATED;
	}
	if (ev.error) {
		size_t len = strnlen(ev.error, MAX_ERROR);
		(void)home_job_erred(job, ev.error, (int)jb_utf8(ev.error, len));
	} else if (e == EPROTO) {
		(void)home_job_erred(job, "tool calls out of order, or too many", -1);
	} else if (e == EILSEQ) {
		(void)home_job_erred(job, "an answer that is not text", -1);
	}
	if (e && job->recovery.error)
		home_job_unpersisted(job);
	if (!(job->flags & DROPPED) && buf_peek(&job->delta).len) {
		job->flags |= DELTA_PENDING;
		job_post(job, job->front, &job->to_front, &job->front_bits, JOB_DELTA);
	}
	return e;
}

/* Take the event parsed in @p job's parser (home_take()), and free its
 * value, the reap step what the step's share does not allow; returns the
 * error that taking it met: ENOMEM if parsing it ran out of memory. An
 * event that is not JSON text errs.
 */
static int
home_event (struct home *home,
            struct job  *job)
{
	struct loop *loop = home->loop;
	struct jb_parser *parser = &job->parser;
	uint32_t n = parser->cut.values;
	struct json_object *root = jb_parser_take(parser);
	bool const memory = parser->error == ENOMEM;
	jb_parser_fini(parser, &home->pool, &home->reaper);
	if (!root) {
		home_reap(home);
		if (memory)
			return ENOMEM;
		(void)home_job_erred(job, "an event that is not JSON text", -1);
		return 0;
	}
	int e = home_take(home, job, root);
	if (n <= loop_room(loop, LOOP_VALUES)) {
		json_object_put(root);
		loop_work(loop, LOOP_VALUES, n);
	} else {
		jb_reaper_add(&home->reaper, root);
	}
	home_reap(home);
	return e;
}

/* Parse the event whole in @p job's wire, a slice a step, take what it says,
 * and free it; then go on past it. 0 once it has, EAGAIN if the step's
 * share ran out first, ECANCELED if the event ended the job. An event that
 * is not JSON text ends it, as a whole answer that is not would. The data
 * [DONE], which is not JSON text, is not parsed. It ends the answer,
 * except in Gemini's Interactions, where it is skipped, because only
 * interaction.completed ends the answer there.
 */
static int
home_read_event (struct home *home,
                 struct job  *job)
{
	char const *data = (char const *)buf_peek(&job->wire).ptr + job->ev;
	int e = 0;
	if (cstr_view_equal(cstr_view_n(data, job->evlen), CSTR_VIEW("[DONE]"))) {
		job->flags |= home->ep->api == AI_INTERACTIONS ? 0 : ENDED;
		if (job->flags & ENDED)
			e = home_thought_end(home, job, true);
	} else if (!home_parse(home, &job->parser, data, job->evlen))
		return EAGAIN;
	else
		e = home_event(home, job);
	buf_consume(&job->wire, job->line);
	job->ev = job->evlen = job->line = job->scan = 0;
	job->flags &= ~EVENT;
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
 * line says nothing, and goes once no event's data precedes it (SSE).
 */
static void
home_job_line (struct job *job,
               size_t      end)
{
	unsigned char *wire = buf_peek(&job->wire).ptr;
	struct cstr_view line = cstr_view_n((char const *)wire + job->line, end - job->line);
	/* The data of a data line ends where the line ends. */
	uint32_t n = (uint32_t)cstr_view_sse_data(&line).len;
	uint32_t at = job->line + (uint32_t)(line.len - n);
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
		job->flags |= line.len ? 0 : EVENT;
		return;
	}
	wire[job->ev + job->evlen++] = '\n';
	memmove(wire + job->ev + job->evlen, wire + at, n);
	job->evlen += n;
}

/* Look for the line break that ends the line of @p job's stream read, as far
 * as the step's share of parsing allows, and take in the line it ends. 0
 * once it has looked, EAGAIN if the share has run out, ENODATA if all of the
 * wire has been looked at already.
 */
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

/* All of @p job's wire has been looked at. A stream that libcurl has not
 * delivered whole has nothing more to read for now (ENODATA): it is off the
 * list of streams to read, and goes on if it was held back for reading. One
 * delivered ends with its last event read, the last line and the last event
 * needing no line breaks (0 until then): cut off, as a transfer cut short,
 * unless an event said the answer had ended (ECANCELED).
 */
static int
home_drained (struct home *home,
              struct job  *job)
{
	if (!(job->flags & DELIVERED)) {
		list_del(&job->feed);
		if (job->flags & BACKED) {
			job->flags &= ~BACKED;
			http_resume(job->transfer);
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
 * (home_drained()): true then, false if the shares ran out first.
 */
static bool
home_read_stream (struct home *home,
                  struct job  *job)
{
	for (;;) {
		if (home_job_waits(job)) {
			list_del(&job->feed);
			return true;
		}
		int e = job->flags & EVENT ? home_read_event(home, job) : home_scan(home, job);
		if (e == ENODATA)
			e = home_drained(home, job);
		if (e)
			return e != EAGAIN;
	}
}

/* Read the streams, each as far as the step's shares allow, the one that
 * used them up going to the back for the next step.
 */
static void
home_feed_step (struct loop      *loop,
                struct loop_step *step)
{
	struct home *home = container_of(step, struct home, feed);
	while (!list_is_empty(&home->feeding)) {
		struct job *job = container_of(home->feeding.next, struct job, feed);
		if (home_read_stream(home, job))
			continue;
		list_move(&home->feeding, &job->feed);
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
 * started, as many a step as its share of requests allows.
 */
static void
home_shed_step (struct loop      *loop,
                struct loop_step *step)
{
	struct home *home = container_of(step, struct home, shed);
	while (home->state == OPEN) {
		struct job *job = home_next(home, false);
		if (!job)
			return;
		if (loop_spent(loop, step, LOOP_REQS))
			return;
		home_refuse(home, job);
		loop_work(loop, LOOP_REQS, 1);
	}
}

/* Cut the upstream call of @p job, a background job that an interactive one
 * preempts, and have it wait again, ready, first of the background jobs, its
 * deadline as it was. A preemption is no outcome of the upstream's, which
 * the breaker counts.
 */
static void
home_preempt (struct home *home,
              struct job  *job)
{
	struct home_metrics *m = home->metrics;
	home_abort(home, job);
	home_unstream(home, job);
	home_job_drop_answer(job);
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
 * as its share of requests allows (home_preempt()).
 */
static void
home_preempt_step (struct loop      *loop,
                   struct loop_step *step)
{
	struct home *home = container_of(step, struct home, preempt);
	for (; !list_is_empty(&home->preempting); loop_work(loop, LOOP_REQS, 1)) {
		if (loop_spent(loop, step, LOOP_REQS))
			return;
		struct job *job = container_of(home->preempting.next, struct job, queue);
		list_del(&job->queue);
		/* Its answer may be all in by now, or the interactive job gone. */
		if (home_job_cuttable(home, job) && home_excess(home)) {
			home_preempt(home, job);
			continue;
		}
		job->flags &= ~PREEMPTED;
		list_append(&home->background, &job->queue);
	}
}

/* Cancel the jobs on home->dropped, or every job on home->timeline while
 * HOME_DRAINING is set. The dropped jobs run upstream, so the timeline
 * contains them too. The function returns when loop_spent() returns true
 * for LOOP_REQS, and the step continues in a later bulk step.
 */
static void
home_stop_step (struct loop      *loop,
                struct loop_step *step)
{
	struct home *home = container_of(step, struct home, stop);
	bool const all = home->flags & HOME_DRAINING;
	struct list *jobs = all ? &home->timeline : &home->dropped;
	for (; !list_is_empty(jobs); loop_work(loop, LOOP_REQS, 1)) {
		if (loop_spent(loop, step, LOOP_REQS))
			return;
		home_cancel(home, all ? container_of(jobs->next, struct job, due)
		                      : container_of(jobs->next, struct job, queue));
	}
}

/* Let the slots of the upstream be @p n, and say so, and why, unless
 * @p why is nullptr: for now.
 */
static void
home_slots (struct home *home,
            uint32_t     n,
            char const  *why)
{
	home->slots = n;
	hist_set(&home->metrics->slots, n);
	http_connections(home->http, n);
	if (why)
		LOG(LOG_INFO, "upstream slots: %" PRIu32 " (%s)", n, why);
	loop_ready_bulk(home->loop, &home->start);
	/* Fewer slots may leave background jobs holding more than they may. */
	home_preempt_excess(home);
}

/* Gather /props's answer, up to MAX_PROPS bytes. */
static size_t
home_props_write (char const *data,
                  size_t      len,
                  void       *user)
{
	struct home *home = user;
	return buf_put(&home->answer, data, len, MAX_PROPS) ? 0 : len;
}

/* /props is over: take @p n slots, saying @p why, unless told the slots,
 * which take all the connections again, /props having had one of its
 * own.
 */
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
 * logged: the default slots (home_props_end()).
 */
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
	if (result)
		home_props_fail(home, "%s",
		                *error ? error : curl_easy_strerror(result));
	else if (status != 200)
		home_props_fail(home, "HTTP %ld", status);
	else
		loop_ready_bulk(home->loop, &home->props);
}

/* Parse /props's answer a slice a step, and take the slots its total_slots
 * says, unless told, and the context its slots' n_ctx says, likewise. The
 * answer of Gemini's model says the context in inputTokenLimit, and no
 * slots.
 */
static void
home_props_step (struct loop      *loop,
                 struct loop_step *step)
{
	struct home *home = container_of(step, struct home, props);
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
	int64_t n = jb_int(total, 0);
	int64_t tokens = jb_int(ctx, 0);
	jb_reaper_add(&home->reaper, root);
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

/* Send GET @p url, whose answer the props step reads. The answer may take
 * PROPS_MS, or the calls' timeout if that is shorter. Returns 0 once
 * asked, else the error of http_post_json().
 */
static int
home_props_get (struct home *home,
                char const  *url)
{
	struct ai_endpoint const *ep = home->ep;
	long ms = ep->timeout_ms > 0 && ep->timeout_ms < PROPS_MS ? ep->timeout_ms : PROPS_MS;
	return http_post_json(home->http, url, nullptr, 0, ep->headers, ms,
	                      home_props_write, home_props_done, home, &home->ask);
}

/* Ask the upstream for the number of its slots at GET /props of its
 * endpoint's server, as llama-server answers, unless the upstream is known
 * to be no llama-server: one of a provider other than llama, or one that
 * uses one of Gemini's APIs. Unless the context size is known, Gemini's API
 * is asked for the context size of the model instead, at GET models/MODEL
 * at the root of the API of the upstream's URL. Returns 0 once asked,
 * ENOTSUP if the endpoint names no model or no such URL exists, else the
 * error of the URL or of the request.
 */
static int
home_props_ask (struct home *home)
{
	struct ai_endpoint const *ep = home->ep;
	if (!ai_props(ep)) {
		dstr_auto url = DSTR_INIT;
		int e = home->context ? ENOENT : ai_model_url(ep, &url);
		return e == ENOENT ? ENOTSUP : e ? e : home_props_get(home, dstr_get(&url));
	}
	CURLU *u = curl_url();
	if (!u)
		return ENOMEM;
	char *url = nullptr;
	int e = curl_url_set(u, CURLUPART_URL, ep->url, 0) ||
	        curl_url_set(u, CURLUPART_PATH, "/props", 0) ||
	        curl_url_set(u, CURLUPART_QUERY, nullptr, 0) ||
	        curl_url_set(u, CURLUPART_FRAGMENT, nullptr, 0) ||
	        curl_url_get(u, CURLUPART_URL, &url, 0) ? EINVAL : home_props_get(home, url);
	curl_free(url);
	curl_url_cleanup(u);
	return e;
}

/* Have the stop step cancel @p job, which runs upstream. */
static void
home_drop (struct home *home,
           struct job  *job)
{
	list_move(&home->dropped, &job->queue);
	loop_ready_bulk(home->loop, &home->stop);
}

/* Puts @p job on the timeline. The job is then in flight. */
static void
home_register (struct home *home,
               struct job  *job)
{
	home_timeline(home, job);
	hist_add(&home->metrics->in_flight, 1);
}

/* Whether a slot is free for a job of class @p c at once: the breaker
 * closed, no job of its class or before it waiting, and, for a background
 * job, the background ones holding fewer slots than they may.
 */
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

/* Admit @p job of class @p c to the queue of its principal @p p, or if
 * @p p is nullptr, of a new one, kept or allocated, at the back of its
 * class's ring; ENOMEM if out of memory. An interactive job that waits with
 * one slot preempts a background one.
 */
static int
home_wait (struct home           *home,
           struct job            *job,
           uint32_t               c,
           struct home_principal *p)
{
	if (!p) {
		struct list *spare = shelf_take(&home->spare);
		p = spare ? container_of(spare, struct home_principal, ring)
		          : malloc(sizeof *p);
		if (!p)
			return ENOMEM;
		*p = (struct home_principal){
			.node.hash = home->flags & HOME_FIFO ? c : job->principal,
			.jobs = list(&p->jobs),
			.class = c
		};
		htab_link(&home->whom, &p->node);
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
 * (home_wait()).
 */
static void
home_admit (struct home *home,
            struct job  *job)
{
	struct home_metrics *m = home->metrics;
	if (home->flags & HOME_DRAINING) {
		home_end(home, job, JOB_CANCELLED, &m->rejected);
		return;
	}
	if (home->state == OPEN) {
		home_refuse(home, job);
		return;
	}
	struct home_principal *who = home_find_whom(home, job);
	if (hist_get(&m->in_flight) >= home->max ||
	    (who && !(home->flags & HOME_FIFO) && who->queued >= JOB_QUEUED)) {
		home_end(home, job, JOB_BUSY, &m->rejected);
		return;
	}
	uint32_t c = home_job_class(job);
	if (home_vacant(home, c)) {
		home_register(home, job);
		list_append(&home->ready[c], &job->queue);
		job->flags |= READY;
		++home->readied[c];
	} else if (home_wait(home, job, c, who)) {
		home_end(home, job, JOB_FAILED, &m->rejected);
		return;
	}
	loop_ready_bulk(home->loop, &home->start);
	/* It may need a descriptor upstream. */
	httpd_home_crowd(home->door);
}

/* @p job comes, with @p bits: a job may come back for another run, and what
 * home noted of the last goes before any end. Home admits it, or ends it
 * cancelled if its front has let go of it already, and lets go too.
 */
static void
home_submit (struct home *home,
             struct job  *job,
             uint32_t     bits)
{
	struct home_metrics *m = home->metrics;
	home_job_reset(job, 0);
	hist_add(&m->submitted, 1);
	if (bits & (JOB_CANCEL | JOB_SUSPEND)) {
		home_end(home, job, JOB_CANCELLED, &m->cancelled);
		home_job_put(job, 2);
		return;
	}
	home_admit(home, job);
	home_job_put(job, 1);
}

/* The front no longer reads the mailbox. A completion held for it can now
 * go; otherwise an acknowledgment lets the parser resume in a bulk step.
 */
static void
home_delta_release (struct job *job)
{
	buf_fini(&job->delta);
	uint32_t const was = job->flags;
	job->flags &= ~(DELTA_PENDING | DELTA_ENDED);
	if (was & DELTA_ENDED)
		home_job_end(job, job->status);
}

/* @p job's front lets go of it: home cancels it if it is in flight, and lets
 * go too, the result's storage back to the stash it came from. Cancelling
 * one running takes a step, which then lets go of it.
 */
static void
home_release (struct home *home,
              struct job  *job,
              uint32_t     bits)
{
	home_delta_release(job);
	job->recovery.flags |= bits & JOB_CANCEL ? RECOVERY_CANCEL : 0;
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
 * if submitted, and nothing counts.
 */
static void
home_job_orphan (struct job *job,
                 uint32_t    bits)
{
	home_delta_release(job);
	/* The reference of the message, and home's reference too if the
	 * front let go of the job. */
	uint32_t const refs = bits & (JOB_CANCEL | JOB_SUSPEND) ? 2 : 1;
	if (bits & JOB_SUBMIT) {
		home_job_reset(job, 0);
		home_job_end(job, JOB_CANCELLED);
	} else if (refs == 2) {
		home_job_drop_answer(job);
	}
	home_job_put(job, refs);
}

void
home_job (struct loop *loop,
          struct msg  *msg)
{
	struct job *job = container_of(msg, struct job, to_home);
	uint32_t bits = atomic_exchange(&job->home_bits, 0);
	if (loop->flags & LOOP_EXIT) {
		home_job_orphan(job, bits);
		return;
	}
	struct home *home = self;
	if (bits & JOB_SUBMIT)
		home_submit(home, job, bits);
	else if (bits & (JOB_CANCEL | JOB_SUSPEND))
		home_release(home, job, bits);
	else if (bits & JOB_DRAIN) {
		home_delta_release(job);
		if (!list_is_empty(&job->due) && job->flags & STREAM)
			home_feed(home, job);
		home_job_put(job, 1);
	}
}

/* With llama, make home->headers: the upstream's header lines without an
 * X-Conversation-Id line, and then home->conversation, which home_open()
 * writes for each job. Returns 0 or ENOMEM.
 */
static int
home_conversation_headers (struct home *home)
{
	char const *const *lines = home->ep->headers;
	size_t n = 0;
	while (lines && lines[n])
		++n;
	char const **headers = calloc(n + 2, sizeof *headers);
	if (!headers)
		return ENOMEM;
	size_t kept = 0;
	for (size_t k = 0; k < n; ++k)
		if (!cstr_view_starts_case(cstr_view(lines[k]), conversation_id))
			headers[kept++] = lines[k];
	headers[kept] = home->conversation;
	home->headers = headers;
	return 0;
}

/* Put @p home's timers in the heap from the start, so that setting them
 * never fails, and make its curl bridge, which counts into @p metrics.
 */
static int
home_arm (struct home         *home,
          struct http_metrics *metrics)
{
	for (size_t i = 0; i < array_size(home->timers); ++i) {
		int e = loop_timer_set(home->loop, &home->timers[i], UINT64_MAX);
		if (e)
			return e;
	}
	int e;
	home->http = http_create(home->loop, metrics, &e);
	return e;
}

/* Free what @p home holds, but for its jobs, however far home_create() got
 * in making it.
 */
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
	}
	struct loop_step *const steps[] = {
		&home->start, &home->stop, &home->overdue, &home->silent,
		&home->feed, &home->reap, &home->shed, &home->preempt, &home->props,
		&home->cleanup,
	};
	for (size_t i = 0; i < array_size(steps); ++i)
		loop_unready(steps[i]);
	for (size_t i = 0; i < array_size(home->timers); ++i)
		loop_timer_stop(home->loop, &home->timers[i]);
	http_destroy(&home->http);
	jb_parser_fini(&home->parser, &home->pool, nullptr);
	jb_reaper_fini(&home->reaper);
	buf_fini(&home->answer);
	free(home->headers);
	home->headers = nullptr;
	while (home_relieve(home, BUF_CRITICAL, SIZE_MAX))
		;
	self = nullptr;
	htab_fini(&home->whom);
}

struct home *
home_create (struct loop         *loop,
             struct blocker      *blockers,
             struct httpd_home   *door,
             struct config const *cfg,
             struct home_metrics *metrics,
             struct http_metrics *http,
             int                 *e)
{
	struct ai_endpoint const *ep = &cfg->ep;
	struct home *home = malloc(sizeof *home);
	if (!home) {
		*e = ENOMEM;
		return nullptr;
	}
	uint32_t max = cfg->calls;
	*home = (struct home){
		.start.fn = home_start_step,
		.stop.fn = home_stop_step,
		.overdue.fn = home_overdue_step,
		.silent.fn = home_silent_step,
		.feed.fn = home_feed_step,
		.reap.fn = home_reap_step,
		.shed.fn = home_shed_step,
		.preempt.fn = home_preempt_step,
		.props.fn = home_props_step,
		.cleanup.fn = home_cleanup_step,
		.timers = {
			[TIMER_DEADLINE].fn = home_deadline_event,
			[TIMER_QUIET].fn = home_quiet_event,
			[TIMER_BREAKER].fn = home_breaker_event,
			[TIMER_CANCEL].fn = home_cleanup_event,
		},
		.pool = {
			.scalar = JB_SCALAR_MAX,
			.flags = JSON_TOKENER_VALIDATE_UTF8
		},
		.metrics = metrics,
		.loop = loop,
		.door = door,
		.ep = ep,
		.timeout = ep->timeout_ms > 0 ? (uint64_t)ep->timeout_ms * MS : 0,
		.silence = ep->chunk_ms > 0 ? (uint64_t)ep->chunk_ms * MS : 0,
		.backoff = BACKOFF_MIN,
		.max = max,
		/* The context size for compacting is the value of --context.
		 * Without --context, it is 0 until home_props_step() sets it
		 * from the answer of /props or of the model URL. */
		.context = ep->context > 0 ? (uint32_t)ep->context : 0,
		.flags = cfg->flags & CFG_TEST_FIFO ? HOME_FIFO : 0
	};
	for (size_t c = 0; c < array_size(home->ring); ++c) {
		list_init(&home->ring[c]);
		list_init(&home->ready[c]);
	}
	list_init(&home->background);
	list_init(&home->preempting);
	list_init(&home->timeline);
	list_init(&home->dropped);
	list_init(&home->idle);
	list_init(&home->feeding);
	list_init(&home->cancellations);
	list_init(&home->spare.list);
	*e = htab_init(&home->whom, stdc_bit_ceil(max < KEYS_MAX ? max : (uint32_t)KEYS_MAX));
	if (!*e)
		*e = home_arm(home, http);
	if (!*e && home_llama(home))
		*e = home_conversation_headers(home);
	if (*e) {
		home_fini(home);
		free(home);
		return nullptr;
	}
	http_resolver(home->http, blockers);
	self = home;
	/* Without --slots, use the default number of slots until the request
	 * to /props, or to the model URL for the Gemini APIs, ends. If the
	 * request cannot start, log the default now. Otherwise
	 * home_props_end() logs the number of slots. */
	if (!ep->slots) {
		home_slots(home, SLOTS, home_props_ask(home) ? "the default" : nullptr);
		return home;
	}
	/* With --slots, request /props or the model URL only for the context
	 * size, if CFG_UPSTREAM_TOOLS is set and the context size is not
	 * known. The connection limit is then the number of slots plus 1, so
	 * that this request does not use a connection of the calls. */
	bool asked = cfg->flags & CFG_UPSTREAM_TOOLS && !home->context &&
	             !home_props_ask(home);
	home_slots(home, ep->slots, "--slots");
	if (asked)
		http_connections(home->http, ep->slots + 1L);
	return home;
}

#undef BACKOFF_MIN

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
	for (size_t i = 0; i < array_size(counts); ++i)
		LOG_ALL(LOG_INFO, "  %-9s  %" PRIu64, counts[i].name, hist_get(counts[i].count));
	home_fini(home);
	free(home);
}

void
home_drain (struct home *home)
{
	home->flags |= HOME_DRAINING;
	loop_ready_bulk(home->loop, &home->stop);
}

void
home_alone (struct home *home)
{
	home->flags |= HOME_ALONE;
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
