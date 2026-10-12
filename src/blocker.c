/** @file Blocker threads and their bounded job queue (blocker.h). */
#define _GNU_SOURCE /* pthread_setname_np() */

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/eventfd.h>

#include "blocker.h"
#include "log.h"
#include "util.h"

enum { QUEUE = 256 }; /* jobs waiting at most */

/* The queue's lock and condition variable are POSIX's, of which C11's mtx_t
 * and cnd_t are wrappers in glibc that ThreadSanitizer does not see. */
struct blocker {
	struct blocker_metrics metrics;
	struct list            queue;
	pthread_mutex_t        lock;
	pthread_cond_t         ready;
	size_t                 queued;
	size_t                 started;
	_Atomic size_t         exited;
	int                    notify_fd;
	int                    stop;       /* under the lock */
};

/* The queue's next job, under the lock, waiting for one; nullptr once the
 * queue is empty and the blockers stop. */
static struct blocker_job *
blocker_next (struct blocker *b)
{
	while (list_is_empty(&b->queue)) {
		if (b->stop)
			return nullptr;
		pthread_cond_wait(&b->ready, &b->lock);
	}
	struct list *next = b->queue.next;
	list_del(next);
	--b->queued;
	return container_of(next, struct blocker_job, hook);
}

/* A blocker thread: run the queue's jobs, or once told to stop, cancel
 * them, until it is empty. */
static void *
blocker_thread (void *arg)
{
	struct blocker *b = arg;
	log_attach();
	(void)pthread_setname_np(pthread_self(), "blocker");
	pthread_mutex_lock(&b->lock);
	for (struct blocker_job *job; (job = blocker_next(b)); ) {
		bool stop = b->stop;
		pthread_mutex_unlock(&b->lock);
		if (stop) {
			job->error = ECANCELED;
		} else {
			job->work(job);
			atomic_fetch_add_explicit(&b->metrics.done, 1, memory_order_relaxed);
		}
		loop_post(job->loop, &job->done);
		pthread_mutex_lock(&b->lock);
	}
	pthread_mutex_unlock(&b->lock);
	/* Once counted out, the blocker may be freed. */
	int notify_fd = b->notify_fd;
	atomic_fetch_add(&b->exited, 1);
	(void)eventfd_write(notify_fd, 1);
	return nullptr;
}

struct blocker *
blocker_create (int  notify_fd,
                int *e)
{
	struct blocker *b = calloc(1, sizeof *b);
	if (!b) {
		*e = ENOMEM;
		return nullptr;
	}
	if ((*e = pthread_mutex_init(&b->lock, nullptr))) {
		free(b);
		return nullptr;
	}
	if ((*e = pthread_cond_init(&b->ready, nullptr))) {
		pthread_mutex_destroy(&b->lock);
		free(b);
		return nullptr;
	}
	atomic_init(&b->metrics.done, 0);
	atomic_init(&b->metrics.refused, 0);
	atomic_init(&b->exited, 0);
	list_init(&b->queue);
	b->notify_fd = notify_fd;
	return b;
}

int
blocker_start (struct blocker *blocker)
{
	while (blocker->started < BLOCKER_THREADS) {
		pthread_t thread;
		int e = pthread_create(&thread, nullptr, blocker_thread, blocker);
		if (e)
			return e;
		(void)pthread_detach(thread);
		++blocker->started;
	}
	return 0;
}

int
blocker_submit (struct blocker     *blocker,
                struct blocker_job *job)
{
	job->error = 0;
	pthread_mutex_lock(&blocker->lock);
	if (blocker->stop || blocker->queued >= QUEUE) {
		pthread_mutex_unlock(&blocker->lock);
		atomic_fetch_add_explicit(&blocker->metrics.refused, 1, memory_order_relaxed);
		return EAGAIN;
	}
	list_append(&blocker->queue, &job->hook);
	++blocker->queued;
	pthread_mutex_unlock(&blocker->lock);
	pthread_cond_signal(&blocker->ready);
	return 0;
}

void
blocker_stop (struct blocker *blocker)
{
	if (!blocker)
		return;
	pthread_mutex_lock(&blocker->lock);
	blocker->stop = true;
	pthread_mutex_unlock(&blocker->lock);
	pthread_cond_broadcast(&blocker->ready);
}

bool
blocker_done (struct blocker const *blocker)
{
	return !blocker || atomic_load(&blocker->exited) == blocker->started;
}

void
blocker_destroy (struct blocker **p_blocker)
{
	if (!p_blocker || !*p_blocker)
		return;
	struct blocker *b = *p_blocker;
	*p_blocker = nullptr;
	pthread_cond_destroy(&b->ready);
	pthread_mutex_destroy(&b->lock);
	free(b);
}

struct blocker_metrics const *
blocker_metrics (struct blocker const *blocker)
{
	return &blocker->metrics;
}
