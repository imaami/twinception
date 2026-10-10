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
struct blockers {
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

static void *
blocker (void *arg)
{
	struct blockers *b = arg;
	log_attach();
	(void)pthread_setname_np(pthread_self(), "blocker");
	pthread_mutex_lock(&b->lock);
	for (;;) {
		while (list_is_empty(&b->queue) && !b->stop)
			pthread_cond_wait(&b->ready, &b->lock);
		if (list_is_empty(&b->queue))
			break;
		struct blocker_job *job = container_of(b->queue.next, struct blocker_job,
		                                       hook);
		list_del(&job->hook);
		--b->queued;
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

int
blockers_start (struct blockers **p_blockers,
                int               notify_fd)
{
	struct blockers *b = calloc(1, sizeof *b);
	if (!b)
		return ENOMEM;
	list_init(&b->queue);
	b->notify_fd = notify_fd;
	int e = pthread_mutex_init(&b->lock, nullptr);
	if (e) {
		free(b);
		return e;
	}
	if ((e = pthread_cond_init(&b->ready, nullptr))) {
		pthread_mutex_destroy(&b->lock);
		free(b);
		return e;
	}
	*p_blockers = b;
	while (!e && b->started < BLOCKER_THREADS) {
		pthread_t thread;
		if (!(e = pthread_create(&thread, nullptr, blocker, b))) {
			pthread_detach(thread);
			++b->started;
		}
	}
	return e;
}

int
blockers_submit (struct blockers    *blockers,
                 struct blocker_job *job)
{
	job->error = 0;
	pthread_mutex_lock(&blockers->lock);
	bool ok = !blockers->stop && blockers->queued < QUEUE;
	if (ok) {
		list_append(&blockers->queue, &job->hook);
		++blockers->queued;
	}
	pthread_mutex_unlock(&blockers->lock);
	if (!ok) {
		atomic_fetch_add_explicit(&blockers->metrics.refused, 1, memory_order_relaxed);
		return EAGAIN;
	}
	pthread_cond_signal(&blockers->ready);
	return 0;
}

void
blockers_stop (struct blockers *blockers)
{
	if (!blockers)
		return;
	pthread_mutex_lock(&blockers->lock);
	blockers->stop = true;
	pthread_mutex_unlock(&blockers->lock);
	pthread_cond_broadcast(&blockers->ready);
}

bool
blockers_done (struct blockers const *blockers)
{
	return !blockers || atomic_load(&blockers->exited) == blockers->started;
}

void
blockers_fini (struct blockers **p_blockers)
{
	struct blockers *b = *p_blockers;
	if (!b)
		return;
	*p_blockers = nullptr;
	pthread_cond_destroy(&b->ready);
	pthread_mutex_destroy(&b->lock);
	free(b);
}

struct blocker_metrics const *
blockers_metrics (struct blockers const *blockers)
{
	return &blockers->metrics;
}
