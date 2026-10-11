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
 * ends it at once (BUSY when too many are in flight), runs it until its
 * deadline at most, writes the result and its status, and posts DONE: every
 * job ends in exactly one completion.  The front posts CANCEL exactly once:
 * before DONE it cancels the job, which home then stops and ends as
 * CANCELLED, and after DONE it lets go of the job, having read the result.
 * Home keeps its reference until then, and gives the result's storage back
 * to its own stash, so that each buffer's storage is taken and given back by
 * one thread.  Whoever drops the last reference returns the job to its
 * front, which keeps it for reuse: home does so through the front's inbox
 * (RETURN).
 *
 * The front names the call that waits for the job by a handle (loop.h), which
 * it frees as it cancels: a completion that finds the handle stale is dropped.
 *
 * A job also carries its call's key, the session and request id a legacy
 * notifications/cancelled names, and the key's hash, by which home finds it
 * in the registry of jobs in flight, whatever loop the notification came to.
 * That loop posts home the key in a job of its own, a carrier, with WITHDRAW:
 * home ends the job it names as WITHDRAWN, and returns the carrier to its
 * front at once.  The front works the hash out, so that home's handling of
 * a message costs the same whatever the key's length.
 */
#ifndef TWINCEPTION_SRC_JOB_H_
#define TWINCEPTION_SRC_JOB_H_

#include <stdatomic.h>
#include <stdint.h>

#include "buf.h"
#include "list.h"
#include "loop.h"

struct http_request;

/** The bits of the messages to home. */
enum {
	JOB_SUBMIT   = 1,  /**< run the job */
	JOB_CANCEL   = 2,  /**< the front lets go: stop the job if it runs */
	JOB_WITHDRAW = 4,  /**< the job is a carrier: end the one its key names,
	                        as its client asked */
};

/** The bits of the messages to the front. */
enum {
	JOB_DONE     = 1,  /**< the job has ended: its status and result are set */
	JOB_PROGRESS = 2,  /**< reserved for S6 */
	JOB_RETURN   = 4,  /**< home dropped the last reference, without one */
};

/** How a job ended. */
enum job_status {
	JOB_ANSWERED,   /**< the result holds the upstream's answer */
	JOB_FAILED,     /**< the result says why the upstream call failed */
	JOB_BUSY,       /**< refused: too many jobs in flight */
	JOB_CANCELLED,  /**< cancelled, or refused by a home that drains; the
	                     result is empty */
	JOB_WITHDRAWN,  /**< cancelled as its client asked, by its key; the
	                     result is empty */
};

/** An upstream call, shared by its front and home (R4). */
struct job {
	struct msg           to_home;     /**< SUBMIT, CANCEL: the front posts it */
	struct msg           to_front;    /**< DONE, RETURN: home posts it */
	struct list          spare;       /**< the front's: on its spare list */
	struct list          queue;       /**< home's: among the jobs in flight */
	struct list          held;        /**< home's: among those whose answer
	                                       waits for a step's share */
	struct buf           request;     /**< the front's: written before SUBMIT,
	                                       read by home until DONE */
	struct buf           result;      /**< home's: written before DONE, read
	                                       by the front until CANCEL */
	struct buf           key;         /**< the front's: written before SUBMIT
	                                       or WITHDRAW, read by home until
	                                       DONE or RETURN: the call's session,
	                                       a line break and its id's JSON */
	struct loop         *front;       /**< the front's loop, set as it is made */
	struct job          *chain;       /**< home's: the next in its registry's
	                                       bucket */
	struct http_request *transfer;    /**< home's: the request upstream */
	struct handle        call;        /**< the front's: what waits for it */
	uint64_t             deadline;    /**< the front writes, before SUBMIT: when
	                                       the call times out, CLOCK_MONOTONIC
	                                       ns; 0: never */
	uint64_t             since;       /**< home's: when it was admitted */
	uint64_t             hash;        /**< the front's, with the key: its hash,
	                                       seeded, which home's registry
	                                       buckets the job by */
	_Atomic uint32_t     home_bits;   /**< pending to home */
	_Atomic uint32_t     front_bits;  /**< pending to the front */
	_Atomic uint32_t     refs;        /**< the front, home, each message queued */
	uint16_t             http;        /**< home writes before DONE: the answer's
	                                       HTTP status */
	uint8_t              status;      /**< home writes before DONE: enum
	                                       job_status */
	uint8_t              flags;       /**< home's: what it notes of the run */
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
	list_init(&job->held);
	job->request = job->result = job->key = (struct buf){};
	job->front = front;
	job->chain = nullptr;
	job->transfer = nullptr;
	job->call = (struct handle){};
	job->deadline = job->since = job->hash = 0;
	job->http = 0;
	job->status = job->flags = 0;
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
