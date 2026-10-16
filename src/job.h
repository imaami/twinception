/** @file Jobs: the upstream calls that client loops hand to home (design
 *  section 4).
 *
 * A job is shared by the loop that asks for it, its front, and home, which
 * runs it: the one mutable object two threads share (R4).  Each field has one
 * writer, named beside it, and each direction has a message embedded in the
 * job with a word of pending bits.  A poster sets its bit with fetch_or(),
 * and only if the word was 0 takes a reference and posts the message; the
 * handler exchanges the word to 0 and acts on every bit it took.  So a burst
 * is one message, and posting never fails.  The references count the front,
 * home and each message queued.
 *
 * The front writes the request and posts SUBMIT.  Home admits the job, or
 * ends it at once (BUSY when too many are in flight or queued for its
 * principal), queues it for an upstream slot, runs it until its deadline at
 * most, writes the result and its status, and posts DONE: every job ends in
 * exactly one completion.  Meanwhile home publishes the job's progress, its
 * place in its principal's queue and then the tokens of its answer, thinking
 * or text, and posts PROGRESS for a front that asked for it, a burst being
 * one message.
 * The front posts CANCEL exactly once: before DONE it cancels the job, which
 * home then stops and ends as CANCELLED, and after DONE it lets go of the
 * job, having read the result.  Home keeps its reference until then, and
 * gives the result's storage back to its own stash, so that each buffer's
 * storage is taken and given back by one thread.  Whoever drops the last
 * reference returns the job to its front, which keeps it for reuse: home does
 * so through the front's inbox (RETURN).
 *
 * The front names the call that waits for the job by a handle (loop.h), which
 * it frees as it cancels: a completion that finds the handle stale is dropped.
 *
 * A job also carries its principal.  The principal is the client's
 * identity, else the call's session, which is the job's key, else the
 * client's address.  The front computes the principal's hash, so that the
 * time that home takes for a message does not depend on the length of the
 * key.  A notifications/cancelled reaches a call through the roster
 * (src/roster.h), not through home.  The call's front then cancels the
 * call's job with CANCEL.
 */
#ifndef TWINCEPTION_SRC_JOB_H_
#define TWINCEPTION_SRC_JOB_H_

#include <stdatomic.h>
#include <stdint.h>

#include "ai.h"
#include "buf.h"
#include "jb.h"
#include "list.h"
#include "loop.h"

struct home_principal;
struct home_delivery_error;
struct home_recovery;
struct http_request;
struct journal;

/** The bits of the messages to home. */
enum {
	JOB_SUBMIT   = 1,  /**< run the job */
	JOB_CANCEL   = 2,  /**< the front lets go: stop the job if it runs */
	JOB_SUSPEND  = 4,  /**< let go on shutdown, leaving durable provider work alive */
};

/** The bits of the messages to the front. */
enum {
	JOB_DONE     = 1,  /**< the job has ended: its status and result are set */
	JOB_PROGRESS = 2,  /**< the job's progress moved on */
	JOB_RETURN   = 4,  /**< home dropped the last reference, without one */
};

/** Tool calls that a streamed answer may make, at most. */
enum { JOB_CALLS = 16 };

/** Jobs that home queues for one principal and one class, at most. */
enum { JOB_QUEUED = 8 };

/** In a running job's progress, beside its tokens: the latest was thinking. */
static constexpr uint32_t JOB_THINKING = UINT32_C(1) << 31;

/** What the front asks of a job, before SUBMIT. */
enum job_ask {
	JOB_NOTIFY     = 1 << 0,  /**< post PROGRESS */
	JOB_BACKGROUND = 1 << 1,  /**< its class is background, not interactive:
	                               interactive jobs go first, keep a slot of
	                               their own, and with one slot preempt it
	                               (home.c) */
	JOB_TOOLS      = 1 << 2,  /**< keep a streamed answer's thinking and tool
	                               calls, which the request offered tools for */
	JOB_DURABLE    = 1 << 3,  /**< detached work whose journal permits recovery */
};

/** The header of a tool call in a streamed answer.  Each call is stored in
 *  its own buffer of job->calls.  The buffer contains this header and then
 *  the bytes of the id, the name, the extra and the JSON text of the
 *  arguments of the call. */
struct job_call {
	uint32_t index;   /**< the index of the call in the answer, or the
	                       number of its buffer if the answer gave none */
	uint32_t replay;  /**< the bytes of the job's replay before it */
	uint32_t id;      /**< bytes of its id */
	uint32_t name;    /**< of its name */
	uint32_t extra;   /**< of the JSON text it carries back (struct ai_call) */
	uint32_t args;    /**< of its arguments */
};

/** The bytes of the call @p c in a job's calls: it and its parts. */
static inline size_t
job_call_size (struct job_call const *c)
{
	return sizeof *c + c->id + c->name + c->extra + c->args;
}

/** How a job ended. */
enum job_status {
	JOB_ANSWERED,   /**< the result holds the upstream's answer, as it came */
	JOB_TEXT,       /**< the result holds the text of a streamed answer */
	JOB_FAILED,     /**< the result says why the upstream call failed */
	JOB_BUSY,       /**< refused: too many jobs in flight, or queued for its
	                     principal */
	JOB_SUSPENDED,  /**< durable stream interrupted: poll to reattach from its journal */
	JOB_CANCELLED,  /**< cancelled, or refused by a home that drains; the
	                     result is empty */
};

/** An upstream call, shared by its front and home (R4). */
struct job {
	struct msg             to_home;     /**< SUBMIT, CANCEL: the front posts it */
	struct msg             to_front;    /**< DONE, PROGRESS, RETURN: home posts it */
	struct list            spare;       /**< the front's: on its spare list */
	struct list            queue;       /**< home's: on its principal's queue, a
	                                         ready list, among the background jobs
	                                         running or those to preempt, or
	                                         among the jobs dropped */
	struct list            due;         /**< home's: among the jobs in flight, by
	                                         deadline */
	struct list            held;        /**< home's: among those whose answer
	                                         waits for a step's share */
	struct list            idle;        /**< home's: among the answers under way,
	                                         by when they last sent */
	struct list            feed;        /**< home's: among the streams to read */
	struct buf             request;     /**< the front's: written before SUBMIT,
	                                         read by home until DONE */
	struct buf             result;      /**< home's: written before DONE, read
	                                         by the front until CANCEL */
	/** Home writes these buffers before DONE, and the front reads them
	 *  until CANCEL.  With JOB_TOOLS, buffer k contains call k of a
	 *  streamed answer (struct job_call).  The calls are numbered in the
	 *  order in which they begin, and the buffers after the last call are
	 *  empty. */
	struct buf             calls[JOB_CALLS];
	struct buf             thought;     /**< likewise: with JOB_TOOLS, its
	                                         thinking's text */
	struct buf             replay;      /**< likewise: with JOB_TOOLS, what its
	                                         turn gives back as it came (struct
	                                         ai_sink) */
	struct buf             key;         /**< The front writes it before SUBMIT,
	                                         and home reads it until DONE.  It
	                                         is the call's session field and a
	                                         line break, or empty if the
	                                         request has no session field. */
	struct buf             wire;        /**< home's: a streamed answer's bytes
	                                         yet to read */
	struct jb_parser       parser;      /**< home's: of the stream's event */
	struct loop           *front;       /**< the front's loop, set as it is made */
	struct http_request   *transfer;    /**< home's: the request upstream */
	char                  *cancel_url;  /**< home's: native background job's
	                                         cancellation URL, or nullptr */
	struct home_principal *who;         /**< home's: whose queue it is on */
	struct home_delivery_error *delivery_error; /**< home's: transfer error kept
	                                               while buffered events drain */
	struct home_recovery  *recovery;    /**< home's: journal and replay state */
	struct journal        *journal;     /**< the front's: shared transcript writer */
	struct handle          call;        /**< the front's: what waits for it */
	uint64_t               deadline;    /**< the front writes, before SUBMIT, when
	                                         the call times out waiting, and home
	                                         as it starts the job, when the
	                                         upstream does: CLOCK_MONOTONIC ns;
	                                         0: never */
	uint64_t               since;       /**< home's: when it started upstream */
	uint64_t               active;      /**< home's: when its answer last sent */
	uint64_t               provider_done; /**< home's: bool, native job is terminal */
	uint64_t               principal;   /**< the front's, with the key: the hash,
	                                         seeded, of whom the call is for */
	uint64_t               peer[2];     /**< the front's: the client's address,
	                                         which is the principal without a
	                                         session; UINT64_MAX twice for an
	                                         identity's */
	_Atomic uint64_t       progress;    /**< home writes, the front reads: the
	                                         answer's tokens << 32 | the place in
	                                         its principal's queue, from 1, or,
	                                         once it runs, JOB_THINKING or 0 */
	_Atomic uint32_t       home_bits;   /**< pending to home */
	_Atomic uint32_t       front_bits;  /**< pending to the front */
	_Atomic uint32_t       refs;        /**< the front, home, each message queued */
	uint32_t               session;     /**< the front's, with the key: the bytes
	                                         of the session and its line break
	                                         that begin it, which tell principals
	                                         apart; 0: none, or the principal is
	                                         an identity */
	uint32_t               ev;          /**< home's: where the event's data
	                                         begins in the wire */
	uint32_t               evlen;       /**< and its length so far */
	uint32_t               line;        /**< where the line read begins */
	uint32_t               scan;        /**< how far it has been looked at */
	uint32_t               chunks;      /**< home's: the answer's tokens, its
	                                         events with text or thinking */
	uint32_t               used;        /**< home writes before DONE: the tokens
	                                         of the request and the answer, as
	                                         its usage says; 0: unknown */
	uint32_t               context;     /**< home writes before DONE: the tokens
	                                         the upstream's context holds, as
	                                         /props says; 0: unknown */
	uint32_t               archive_turn; /**< the front's: provider turn number */
	struct ai_reader       reader;      /**< home's: of the stream's events */
	uint16_t               http;        /**< home writes before DONE: the answer's
	                                         HTTP status */
	uint16_t               flags;       /**< home's: what it notes of the run */
	uint8_t                status;      /**< home writes before DONE: enum
	                                         job_status */
	uint8_t                asks;        /**< the front's, before SUBMIT: enum
	                                         job_ask */
	char                   reason[42];  /**< home's: the answer's stop reason,
	                                         longer than any the providers
	                                         name */
	char                   archive[33]; /**< the front's: transcript job id */
};

/** Home's handler of the messages to it (home.c). */
extern void
home_job (struct loop *loop,
          struct msg  *msg);

/**
 * Initialize @p job, storage just allocated (hist.h on atomics), for the loop
 * @p front, whose handler of the messages to it is @p done.
 */
static inline void
job_init (struct job  *job,
          struct loop *front,
          void       (*done) (struct loop *loop,
                              struct msg  *msg))
{
	job->to_home = (struct msg){ .fn = home_job };
	job->to_front = (struct msg){ .fn = done };
	list_init(&job->spare);
	list_init(&job->queue);
	list_init(&job->due);
	list_init(&job->held);
	list_init(&job->idle);
	list_init(&job->feed);
	job->request = job->result = job->thought = job->replay = (struct buf){};
	for (size_t k = 0; k < JOB_CALLS; ++k)
		job->calls[k] = (struct buf){};
	job->key = job->wire = (struct buf){};
	job->parser = (struct jb_parser){};
	job->front = front;
	job->transfer = nullptr;
	job->cancel_url = nullptr;
	job->who = nullptr;
	job->delivery_error = nullptr;
	job->recovery = nullptr;
	job->journal = nullptr;
	job->call = (struct handle){};
	job->deadline = job->since = job->active = job->principal = 0;
	job->peer[0] = job->peer[1] = 0;
	job->session = job->ev = job->evlen = job->line = job->scan = job->chunks = 0;
	job->used = job->context = 0;
	job->archive_turn = 0;
	job->reader = (struct ai_reader){};
	job->http = job->flags = 0;
	job->status = job->asks = 0;
	job->provider_done = false;
	job->reason[0] = '\0';
	job->archive[0] = '\0';
	atomic_init(&job->progress, 0);
	atomic_init(&job->home_bits, 0);
	atomic_init(&job->front_bits, 0);
	atomic_init(&job->refs, 0);
}

/** Set @p bit in @p word, and post @p msg to @p loop with a reference if
 *  that made the word non-zero. */
static inline void
job_post (struct job       *job,
          struct loop      *loop,
          struct msg       *msg,
          _Atomic uint32_t *word,
          uint32_t          bit)
{
	if (!atomic_fetch_or(word, bit)) {
		/* The post publishes it before the handler can drop it. */
		atomic_fetch_add_explicit(&job->refs, 1, memory_order_relaxed);
		loop_post(loop, msg);
	}
}

/** Drop @p n references; true if they were the last, which leaves the job
 *  to the caller alone. */
static inline bool
job_put (struct job *job,
         uint32_t    n)
{
	return atomic_fetch_sub_explicit(&job->refs, n, memory_order_acq_rel) == n;
}

#endif /* TWINCEPTION_SRC_JOB_H_ */
