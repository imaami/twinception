/** @file askai: an MCP server whose tool asks another AI, and whose other,
 *  optional one reads the manual. */
#include <errno.h>
#include <inttypes.h>
#include <malloc.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>

#include "ai.h"
#include "blocker.h"
#include "brainstem.h"
#include "buf.h"
#include "config.h"
#include "git.h"
#include "home.h"
#include "http.h"
#include "httpd.h"
#include "log.h"
#include "loop.h"
#include "manual.h"
#include "mcp.h"
#include "metrics.h"
#include "notes.h"
#include "task.h"
#include "util.h"

/* The phases of a relief of memory pressure. */
enum { RELIEVE_KEPT, RELIEVE_STASH, RELIEVE_END, RELIEVED };

/* glibc's tuning (see serve()): blocks from this size up get mappings of
 * their own, and where nothing limits the address space, heaps keep this
 * much of their tops; and a free of this much trims the top of its arena's
 * heap. */
enum { MMAP_MIN = 32 << 20, TOP_PAD = 64 << 20, TOP_FREE = 64 << 10 };

/* The heaps' top pad: glibc's default, or TOP_PAD (see serve()). */
static int top_pad = 128 << 10;

/* Blocks of TOP_FREE, one from each loop's arena that relieved memory
 * pressure, chained through their first bytes, for the trim to free. */
static _Atomic(void *) tops;

/* Free @p block under a low threshold and pad (glibc), which gives back the
 * top of the arena it came from, as only such a free does.  The settings are
 * the process's, lowered for this free alone, and only one thread at a time
 * does it (brainstem_relieved()). */
static void
top_trim (void *block)
{
	mallopt(M_TOP_PAD, 0);
	mallopt(M_TRIM_THRESHOLD, 0);
	free(block);
	mallopt(M_TRIM_THRESHOLD, -1);
	mallopt(M_TOP_PAD, top_pad);
}

/* Whether the kernel commits memory strictly (vm.overcommit_memory 2). */
static bool
strict_commit (void)
{
	FILE *f = fopen("/proc/sys/vm/overcommit_memory", "re");
	int mode = f ? fgetc(f) : EOF;
	if (f)
		fclose(f);
	return mode == '2';
}

/* What a loop thread keeps for reuse, and its reliefs of memory pressure
 * (design section 5); first in each loop's app, which it stands for. */
struct keep {
	struct buf_stash    stash;
	struct loop_step    relief;
	struct loop_step    trim;      /* the trim, on the last loop of a relief */
	struct loop_timer   age;       /* the windows of reuse */
	struct mcp         *mcp;       /* what a client loop keeps besides */
	struct httpd_home  *door;      /* what home keeps besides */
	struct home        *home;      /* and its scheduler */
	struct blocker     *blockers;  /* where the walk goes */
	void               *tops;      /* the trim's blocks yet to free */
	uint64_t            released;  /* the stash's count as the relief began */
	uint64_t            trim_ns;   /* the trim's so far */
	uint64_t            phase;     /* the relief's, widened over the padding */
};

/* The trim's walk of the arenas, on a blocker, for the keep of the loop that
 * trims, and its time, which the blocker writes: here, as one trim at a time
 * walks, and a blocker still busy at shutdown is left to the process's exit,
 * after the loops' keeps are gone. */
static struct {
	struct blocker_job  job;
	struct keep        *keep;
	uint64_t            ns;
} walker;

/* A reload of the configuration, on SIGHUP with a configuration file or TLS:
 * a blocker reads the command line, the file and the TLS certificate and key
 * again, then home hands the client loops the new policy (src/policy.h),
 * and its door the new TLS context; the other options take a restart, which
 * askai says.  One at a time, a SIGHUP meanwhile having another follow. */
struct reload {
	struct blocker_job job;
	struct config      cfg;     /* as the blocker read it */
	int                status;  /* config_reload()'s */
	uint16_t           busy;    /* bool */
	uint16_t           again;   /* bool */
};

/* What runs on home: the jobs (src/home.h), and the door, which accepts the
 * clients and places them on the client loops (src/httpd.h). */
struct home_app {
	struct keep               keep;
	struct reload             reload;
	struct http_metrics       http;  /* here, as the client loops render it */
	struct home_metrics       jobs;  /* likewise */
	struct ai_endpoint const *ep;
	struct httpd_cfg const   *hcfg;
	struct httpd_hub         *hub;
	struct metrics_src       *metrics;
	struct config const      *cfg;
	struct loop              *loop;  /* home's: the client loops' jobs go there */
};

/* What runs on a client loop: MCP over HTTP. */
struct app {
	struct keep              keep;
	struct metrics_mcp       counts;  /* MCP's, which any loop renders */
	struct home_app const   *home;
	struct mcp_static const *st;
	struct httpd_cfg const  *hcfg;
	struct httpd_hub        *hub;
	struct metrics_src      *metrics;
	struct httpd            *httpd;
	struct task_pool        *tasks;   /* if askai offers test_task */
};

/*
 * Relieve memory pressure, in bulk steps (design section 5): free the
 * requests and jobs, or connections, kept for reuse, then the stash's
 * storage, as much of each a step as its share of release work allows and
 * as much in all as the level says; then leave a block for the trim, which
 * the last loop of all to end its share runs, in a step of its own, as that
 * one call cannot be split.  The share ends early if the pressure clears.
 */
static void
relief_step (struct loop      *loop,
             struct loop_step *step)
{
	struct keep *keep = container_of(step, struct keep, relief);
	enum buf_pressure level = atomic_load_explicit(&buf_pressure, memory_order_relaxed);
	size_t room = loop_room(loop, LOOP_RELEASE);
	size_t n;
	switch (level || keep->phase == RELIEVED ? keep->phase : RELIEVE_END) {
	case RELIEVE_KEPT:
		n = keep->mcp ? mcp_relieve(keep->mcp, level, room)
		  : keep->door ? httpd_home_relieve(keep->door, level, room) : 0;
		if (!n && keep->home)
			n = home_relieve(keep->home, level, room);
		break;
	case RELIEVE_STASH:
		n = buf_stash_release(&keep->stash, level, room);
		break;
	case RELIEVE_END: {
		keep->phase = RELIEVED;
		void *block = malloc(TOP_FREE);
		void *head = atomic_load_explicit(&tops, memory_order_relaxed);
		if (block) {
			do memcpy(block, &head, sizeof head);
			while (!atomic_compare_exchange_weak(&tops, &head, block));
		}
		if (brainstem_relieved(hist_get(&keep->stash.metrics.released) -
		                       keep->released)) {
			keep->tops = atomic_exchange(&tops, nullptr);
			keep->trim_ns = 0;
			loop_ready_bulk(loop, &keep->trim);
		}
		return;
	}
	default:
		return;
	}
	loop_work(loop, LOOP_RELEASE, n);
	keep->phase += !n;
	loop_ready_bulk(loop, step);
}

/* Walk every arena with malloc_trim() (glibc), which gives back the free
 * pages within their heaps and the top of the main arena's, whatever the
 * settings: on a blocker, as it takes each arena's lock in turn, so that a
 * loop that allocates meanwhile waits only for its own arena's part, where
 * on a loop the walk of them all would be one step. */
static void
walk (struct blocker_job *job)
{
	(void)job;
	uint64_t start = loop_clock();
	malloc_trim(0);
	walker.ns = loop_clock() - start;
}

/* The trim is over: count it, and let the next relief begin. */
static void
trim_end (struct keep *keep)
{
	struct buf_stash_metrics *m = &keep->stash.metrics;
	hist_add(&m->trims, 1);
	hist_add(&m->trim_ns, keep->trim_ns);
	uint64_t bytes = brainstem_trimmed();
	/* A relief that found little to do is routine while pressure lasts. */
	LOG(bytes || keep->trim_ns >= 1'000'000 ? LOG_INFO : LOG_VERBOSE, "memory "
	    "pressure relieved: %.1f MiB of buffers released, then a trim of %.1f ms",
	    (double)bytes / (1 << 20), (double)keep->trim_ns / 1e6);
}

static void
walk_done (struct loop *loop,
           struct msg  *msg)
{
	(void)msg;
	if (loop->exit)
		return;
	walker.keep->trim_ns += walker.job.error ? 0 : walker.ns;
	trim_end(walker.keep);
}

/* The last loop of a relief trims the allocator for every loop: a bulk step
 * for each loop's block, which gives back that arena's top, then the walk of
 * them all on a blocker, or here if none takes it.  The pressure cleared,
 * the blocks are only freed. */
static void
trim_step (struct loop      *loop,
           struct loop_step *step)
{
	struct keep *keep = container_of(step, struct keep, trim);
	bool on = atomic_load_explicit(&buf_pressure, memory_order_relaxed);
	void *block = keep->tops;
	if (block) {
		memcpy(&keep->tops, block, sizeof keep->tops);
		uint64_t start = loop_clock();
		if (on)
			top_trim(block);
		else
			free(block);
		keep->trim_ns += loop_clock() - start;
		loop_ready_bulk(loop, step);
	} else if (!on) {
		(void)brainstem_trimmed();
	} else {
		walker.job = (struct blocker_job){
			.done.fn = walk_done,
			.work = walk,
			.loop = loop
		};
		walker.keep = keep;
		if (blocker_submit(keep->blockers, &walker.job)) {
			walk(&walker.job);
			keep->trim_ns += walker.ns;
			trim_end(keep);
		}
	}
}

/* Begin a window of reuse, over which a relief at high pressure finds what
 * went unused: as long as the brainstem waits between reliefs. */
static void
keep_age (struct loop       *loop,
          struct loop_timer *timer)
{
	struct keep *keep = container_of(timer, struct keep, age);
	buf_stash_age(&keep->stash);
	if (keep->mcp)
		mcp_age(keep->mcp);
	if (keep->door)
		httpd_home_age(keep->door);
	if (keep->home)
		home_age(keep->home);
	(void)loop_timer_set(loop, timer, loop->now + BRAINSTEM_RELIEF_MS * 1'000'000ULL);
}

/* The loop's app, whose keep comes first. */
static void
keep_relieve (void        *ctx,
              struct loop *loop)
{
	struct keep *keep = ctx;
	keep->phase = RELIEVE_KEPT;
	keep->released = hist_get(&keep->stash.metrics.released);
	hist_add(&keep->stash.metrics.reliefs, 1);
	loop_ready_bulk(loop, &keep->relief);
}

/* Give the calling thread @p keep's stash, and begin its windows of reuse;
 * a trim walks the arenas on @p blockers. */
static int
keep_start (struct keep     *keep,
            struct loop     *loop,
            struct blocker  *blockers)
{
	keep->blockers = blockers;
	keep->relief.fn = relief_step;
	keep->trim.fn = trim_step;
	keep->age.fn = keep_age;
	keep->phase = RELIEVED;
	buf_stash_attach(&keep->stash);
	return loop_timer_set(loop, &keep->age,
	                      loop->now + BRAINSTEM_RELIEF_MS * 1'000'000ULL);
}

/* Give back all that @p keep's stash keeps, and the blocks of a trim cut
 * short, and leave the thread without. */
static void
keep_stop (struct keep *keep,
           struct loop *loop)
{
	for (void *block = keep->tops; block; block = keep->tops) {
		memcpy(&keep->tops, block, sizeof keep->tops);
		free(block);
	}
	loop_unready(&keep->relief);
	loop_unready(&keep->trim);
	loop_timer_stop(loop, &keep->age);
	(void)buf_stash_release(&keep->stash, BUF_CRITICAL, SIZE_MAX);
	buf_stash_attach(nullptr);
}

static int
home_app_start (void            *ctx,
                struct loop     *loop,
                struct blocker  *blockers)
{
	struct home_app *app = ctx;
	struct metrics_src *metrics = app->metrics;
	struct metrics_loop *mine = &metrics->loops[metrics->n_loops - 1];
	app->loop = loop;
	snprintf(mine->name, sizeof mine->name, "home");
	mine->loop = loop->metrics;
	mine->stash = &app->keep.stash.metrics;
	metrics->blockers = blocker_metrics(blockers);
	int e = keep_start(&app->keep, loop, blockers);
	if (!e)
		e = httpd_home_init(&app->keep.door, loop, app->hub, app->hcfg,
		                    &app->jobs.in_flight);
	if (!e)
		e = home_init(&app->keep.home, loop, blockers, app->keep.door, app->ep,
		              app->cfg, &app->jobs, &app->http);
	if (e)
		LOG(LOG_ERROR, "cannot start home: %s", strerror(e));
	return e;
}

static void
home_app_open (void        *ctx,
               struct loop *loop)
{
	(void)loop;
	httpd_home_open(((struct home_app *)ctx)->keep.door);
}

static void
home_app_drain (void        *ctx,
                struct loop *loop,
                uint64_t     deadline)
{
	(void)loop;
	(void)deadline;
	struct home_app *app = ctx;
	httpd_home_drain(app->keep.door);
	home_drain(app->keep.home);
}

static void
home_app_alone (void        *ctx,
                struct loop *loop)
{
	(void)loop;
	home_alone(((struct home_app *)ctx)->keep.home);
}

static void reload_done (struct loop *loop, struct msg *msg);

static void
reload_work (struct blocker_job *job)
{
	struct reload *r = container_of(job, struct reload, job);
	struct home_app const *app = container_of(r, struct home_app, reload);
	r->status = config_reload(&r->cfg, app->cfg);
}

static void
home_app_reload (void        *ctx,
                 struct loop *loop)
{
	struct home_app *app = ctx;
	struct reload *r = &app->reload;
	if (r->busy) {
		r->again = true;
		return;
	}
	r->job = (struct blocker_job){
		.done.fn = reload_done,
		.work = reload_work,
		.loop = loop
	};
	r->busy = !blocker_submit(app->keep.blockers, &r->job);
	if (!r->busy)
		LOG(LOG_WARN, "configuration not reloaded: the blockers are busy");
}

/* The blocker has read the configuration: hand the client loops its
 * policy, and the door its TLS context, unless it is wrong, as the log says
 * then.  A context made only as the file now names a TLS listener serves
 * none until a restart. */
static void
reload_done (struct loop *loop,
             struct msg  *msg)
{
	struct reload *r = container_of(msg, struct reload, job.done);
	struct home_app *app = container_of(r, struct home_app, reload);
	r->busy = false;
	/* Cancelled, as the blockers stop, the work was not done. */
	if (r->job.error)
		return;
	if (!loop->exit) {
		int e = r->status < 0 ? httpd_home_policy(app->keep.door, r->cfg.policy)
		                      : 0;
		bool tls = r->status < 0 && !e && r->cfg.tls && app->cfg->tls;
		if (tls)
			httpd_home_tls(app->keep.door, r->cfg.tls);
		if (r->status >= 0)
			LOG(LOG_ERROR, "configuration not reloaded; the last stays");
		else if (e)
			LOG(LOG_ERROR, "configuration not reloaded: %s; the last stays",
			    e == EAGAIN ? "the server is not open" : strerror(e));
		else
			LOG(LOG_INFO, "configuration reloaded: who may connect, and what "
			    "they may do%s%s", tls ? ", and the TLS certificate" : "",
			    r->cfg.restart ? "; its other options take a restart" : "");
	}
	config_fini(&r->cfg);
	if (r->again && !loop->exit) {
		r->again = false;
		home_app_reload(app, loop);
	}
}

static void
home_app_stop (void        *ctx,
               struct loop *loop)
{
	struct home_app *app = ctx;
	home_fini(&app->keep.home);
	httpd_home_fini(&app->keep.door);
	keep_stop(&app->keep, loop);
}

static int
app_start (void            *ctx,
           struct loop     *loop,
           struct blocker  *blockers)
{
	struct app *app = ctx;
	struct metrics_loop *mine = &app->metrics->loops[loop->id - 1];
	snprintf(mine->name, sizeof mine->name, "%" PRIu64, loop->id);
	mine->loop = loop->metrics;
	mine->stash = &app->keep.stash.metrics;
	mine->kept = &app->counts.kept;
	mine->calls = &app->counts.calls;
	mine->methods = app->counts.methods;
	mine->conns = httpd_hub_metrics(app->hub, loop->id - 1);
	if (app->tasks) {
		task_pool_attach(app->tasks, loop);
		mine->tasks = task_pool_loop_metrics(app->tasks, loop->id - 1);
	}
	int e = keep_start(&app->keep, loop, blockers);
	if (!e)
		e = mcp_init(&app->keep.mcp, loop, app->home->loop, app->st, app->metrics,
		             &app->counts);
	if (e)
		LOG(LOG_ERROR, "cannot start client loop %" PRIu64 ": %s", loop->id,
		    strerror(e));
	else
		e = httpd_init(&app->httpd, loop, app->hub, loop->id - 1,
		               mcp_app(app->keep.mcp), app->hcfg);
	return e;
}

static void
app_drain (void        *ctx,
           struct loop *loop,
           uint64_t     deadline)
{
	(void)loop;
	struct app *app = ctx;
	httpd_drain(app->httpd, deadline);
	mcp_drain(app->keep.mcp);
}

static void
app_stop (void        *ctx,
          struct loop *loop)
{
	struct app *app = ctx;
	if (app->tasks)
		task_pool_detach(app->tasks, loop);
	httpd_fini(&app->httpd);
	mcp_fini(&app->keep.mcp);
	keep_stop(&app->keep, loop);
}

/* Serve until shut down, as @p cfg says; returns the exit status. */
static int
serve (struct config const *cfg)
{
	/* Keep what is freed for reuse, and give memory back to the system only
	 * under memory pressure (design section 5).  glibc would otherwise map
	 * and unmap each block over 128 KiB of its own, give back the top of a
	 * heap as it is freed and a thread's heap once all of it is, and grow a
	 * thread's heap a page and an mprotect() at a time.  Blocks of up to
	 * 32 MiB, the most the threshold takes and the largest stash class, come
	 * from the heaps.  A top pad as large as a heap keeps a thread's heaps,
	 * and makes each writable whole as glibc creates it; as that counts
	 * against RLIMIT_DATA, RLIMIT_AS and a strict commit limit, under any of
	 * those the pad stays glibc's. */
	mallopt(M_MMAP_THRESHOLD, MMAP_MIN);
	mallopt(M_TRIM_THRESHOLD, -1);
	struct rlimit data;
	struct rlimit space;
	if (!getrlimit(RLIMIT_DATA, &data) && data.rlim_cur == RLIM_INFINITY &&
	    !getrlimit(RLIMIT_AS, &space) && space.rlim_cur == RLIM_INFINITY &&
	    !strict_commit())
		top_pad = TOP_PAD;
	mallopt(M_TOP_PAD, top_pad);

	/* Only descriptors limit the connections: take all the hard limit allows. */
	struct rlimit nofile;
	if (!getrlimit(RLIMIT_NOFILE, &nofile) && nofile.rlim_cur < nofile.rlim_max) {
		nofile.rlim_cur = nofile.rlim_max;
		(void)setrlimit(RLIMIT_NOFILE, &nofile);
	}

	/* What every loop shares is made before any starts: the immutable, and
	 * what home's door shares with the client loops' servers.  Each loop
	 * sets its metrics as it starts, and home opens its door only once all
	 * have, so that no request renders them before. */
	struct brainstem_cfg bcfg = cfg->brainstem;
	size_t n = bcfg.loops;
	struct manual *manual = nullptr;
	struct mcp_static *st = nullptr;
	struct httpd_hub *hub = nullptr;
	struct git *git = cfg->n_gits ? git_new(cfg->gits, cfg->n_gits, cfg->git_timeout)
	                              : nullptr;
	int git_error = cfg->n_gits && !git ? errno : 0;
	char why[256];
	struct notes *notes = cfg->notes ? notes_new(cfg->notes, why, sizeof why) : nullptr;
	if (cfg->notes && !notes) {
		fprintf(stderr, "askai: --notes: %s: %s\n", cfg->notes, why);
		git_free(&git);
		return 1;
	}
	int e = git_error;
	struct task_pool *tasks = cfg->flags & CFG_TEST_TASK ? task_pool_create(n, &e)
	                                                     : nullptr;
	struct app *apps = calloc(n, sizeof *apps);
	struct metrics_loop *loops = calloc(n + 1, sizeof *loops);
	if (!apps || !loops)
		e = ENOMEM;
	if (!e && cfg->flags & CFG_MAN)
		e = manual_new(&manual, cfg->man_recheck);
	if (!e)
		e = mcp_static_new(&st, cfg, manual, tasks, git, notes);
	if (!e)
		e = httpd_hub_new(&hub, n);
	struct metrics_src metrics = {
		.loops = loops,
		.man = manual ? manual_metrics(manual) : nullptr,
		.tasks = tasks ? task_pool_metrics(tasks) : nullptr,
		.n_loops = n + 1
	};
	struct home_app home = {
		.ep = &cfg->ep,
		.hcfg = &cfg->http,
		.hub = hub,
		.metrics = &metrics,
		.cfg = cfg
	};
	metrics.http = &home.http;
	metrics.jobs = &home.jobs;
	for (size_t i = 0; !e && i < n; ++i) {
		struct app *app = &apps[i];
		buf_stash_init(&app->keep.stash);
		metrics_mcp_init(&app->counts);
		app->home = &home;
		app->st = st;
		app->hcfg = &cfg->http;
		app->hub = hub;
		app->metrics = &metrics;
		app->tasks = tasks;
	}
	bcfg.home = (struct brainstem_app){
		.ctx = &home,
		.start = home_app_start,
		.open = home_app_open,
		.drain = home_app_drain,
		.relieve = keep_relieve,
		.alone = home_app_alone,
		.reload = cfg->path || cfg->tls ? home_app_reload : nullptr,
		.stop = home_app_stop
	};
	bcfg.client = (struct brainstem_app){
		.ctx = apps,
		.stride = sizeof *apps,
		.start = app_start,
		.drain = app_drain,
		.relieve = keep_relieve,
		.stop = app_stop
	};
	log_threshold = cfg->flags & CFG_VERBOSE ? LOG_VERBOSE : LOG_INFO;
	int status = e ? 1 : brainstem_run(&bcfg);
	if (e)
		fprintf(stderr, "askai: %s\n", strerror(e));
	if (tasks && !e) {
		/* Every loop has stopped, and the counts are whole, but where a
		 * blocker still busy kept the loops unfinished, their inboxes
		 * unread: the tasks those held are under way still. */
		struct task_pool_metrics const *t = task_pool_metrics(tasks);
		uint64_t moved = 0;
		for (size_t i = 0; i < n; ++i)
			moved += hist_get(&task_pool_loop_metrics(tasks, i)->moved);
		uint64_t submitted = hist_get(&t->submitted);
		uint64_t ended = hist_get(&t->done) + hist_get(&t->cancelled) +
		                 hist_get(&t->abandoned);
		char rest[48] = "";
		if (submitted > ended)
			snprintf(rest, sizeof rest, "; %" PRIu64 " under way", submitted - ended);
		fprintf(stderr, "askai: tasks: %" PRIu64 " submitted, %" PRIu64 " done, %"
		        PRIu64 " cancelled, %" PRIu64 " abandoned; %" PRIu64 " moved%s\n",
		        submitted, hist_get(&t->done), hist_get(&t->cancelled),
		        hist_get(&t->abandoned), moved, rest);
	}
	task_pool_destroy(&tasks);
	httpd_hub_free(&hub);
	git_free(&git);
	notes_free(&notes);
	mcp_static_free(&st);
	manual_free(&manual);
	free(loops);
	free(apps);
	return status;
}

int
main (int   argc,
      char *argv[])
{
	int status;
	struct config cfg = config(argc, argv, &status);
	if (status < 0)
		status = serve(&cfg);
	config_fini(&cfg);
	return status;
}
