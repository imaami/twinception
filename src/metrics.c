/** @file Prometheus text exposition of the core's metrics (metrics.h). */
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>

#include "blocker.h"
#include "brainstem.h"
#include "buf.h"
#include "home.h"
#include "http.h"
#include "log.h"
#include "loop.h"
#include "metrics.h"
#include "util.h"

/* What a call renders at most, a histogram costing 8 and a value 1: a
 * histogram's buckets take a pass over all of its own. */
enum { BUDGET = 32 };

/* Where a series is: those before PER_LOOP are each loop's, rendered for
 * every loop that has them, labelled with its name. */
enum source {
	LOOP, STASH, KEPT, CALLS, METHODS, CONNS, PER_LOOP,
	HTTP = PER_LOOP, BLOCKERS, JOBS, BRAINSTEM, LOGGER, PSI, CGROUP, SOURCES
};
enum kind { COUNTER, GAUGE, HISTOGRAM };
enum scale { ONE, NS, US, PERMYRIAD };

/* A series, or a loop's; help, given for the first of a family only, starts
 * the family. */
struct entry {
	char const *name;
	char const *help;
	char const *labels;  /* within the braces, but for a loop's, or "" */
	uint32_t    off;     /* into its source */
	uint8_t     source;
	uint8_t     kind;
	uint16_t    scale;
};

static char const *const kinds[] = { "counter", "gauge", "histogram" };

static struct {
	uint64_t    ns;
	char const *le;
} const bounds[] = {
	{ 1'000, "1e-06" },               { 10'000, "1e-05" },
	{ 25'000, "2.5e-05" },            { 50'000, "5e-05" },
	{ 100'000, "0.0001" },            { 250'000, "0.00025" },
	{ 500'000, "0.0005" },            { 1'000'000, "0.001" },
	{ 2'500'000, "0.0025" },          { 5'000'000, "0.005" },
	{ 10'000'000, "0.01" },           { 25'000'000, "0.025" },
	{ 50'000'000, "0.05" },           { 100'000'000, "0.1" },
	{ 250'000'000, "0.25" },          { 500'000'000, "0.5" },
	{ 1'000'000'000, "1" },           { 2'500'000'000, "2.5" },
	{ 5'000'000'000, "5" },           { 10'000'000'000, "10" },
	{ 30'000'000'000, "30" },         { 60'000'000'000, "60" },
	{ 300'000'000'000, "300" },
};

#define LOOP_HIST(name, help, labels, field) \
	{ name, help, labels, offsetof(struct loop_metrics, field), LOOP, HISTOGRAM, NS }
#define WORK(help, unit, u) \
	{ "askai_loop_step_work_max", help, "unit=\"" unit "\"", \
	  offsetof(struct loop_metrics, work[u]), LOOP, GAUGE, ONE }
#define METHOD(help, method, m) \
	{ "askai_request_seconds", help, "method=\"" method "\"", \
	  (m) * sizeof (struct hist), METHODS, HISTOGRAM, NS }
#define VALUE(name, help, labels, type, field, source, kind, scale) \
	{ name, help, labels, offsetof(type, field), source, kind, scale }
#define STASHED(name, help, labels, field, kind, scale) \
	VALUE(name, help, labels, struct buf_stash_metrics, field, STASH, kind, scale)
#define KEPT(name, help, labels, field) \
	VALUE(name, help, labels, struct metrics_kept, field, KEPT, GAUGE, ONE)
#define CALLS(name, help, field) \
	VALUE(name, help, "", struct metrics_calls, field, CALLS, COUNTER, ONE)
#define CONNS(name, help, field, kind) \
	VALUE(name, help, "", struct metrics_conns, field, CONNS, kind, ONE)
#define JOBS(name, help, labels, field, kind) \
	VALUE(name, help, labels, struct home_metrics, field, JOBS, kind, ONE)
#define INPUT(help, input, i) \
	VALUE("askai_memory_pressure_input_level", help, "input=\"" input "\"", \
	      struct brainstem_metrics, inputs[i], BRAINSTEM, GAUGE, ONE)

static struct entry const table[] = {
	LOOP_HIST("askai_loop_turn_seconds", "Time an event loop turn took.", "", turn),
	LOOP_HIST("askai_loop_step_seconds", "Time a step took.",
	          "kind=\"interactive\"", step[0]),
	LOOP_HIST("askai_loop_step_seconds", nullptr, "kind=\"bulk\"", step[1]),
	LOOP_HIST("askai_loop_timer_lateness_seconds", "How late a timer fired.", "",
	          late),
	LOOP_HIST("askai_loop_inbox_delay_seconds",
	          "Time from a message's post to its handler.", "", inbox),
	WORK("The most of a unit of work one step did (design section 1).", "reqs",
	     LOOP_REQS),
	WORK(nullptr, "recv", LOOP_RECV),
	WORK(nullptr, "send", LOOP_SEND),
	WORK(nullptr, "body", LOOP_BODY),
	WORK(nullptr, "parse", LOOP_PARSE),
	WORK(nullptr, "values", LOOP_VALUES),
	WORK(nullptr, "escape", LOOP_ESCAPE),
	WORK(nullptr, "copy", LOOP_COPY),
	WORK(nullptr, "accepts", LOOP_ACCEPTS),
	WORK(nullptr, "closes", LOOP_CLOSES),
	WORK(nullptr, "release", LOOP_RELEASE),
	METHOD("Time from a request's head to its end, by MCP method.", "initialize",
	       METHOD_INITIALIZE),
	METHOD(nullptr, "ping", METHOD_PING),
	METHOD(nullptr, "tools/list", METHOD_TOOLS_LIST),
	METHOD(nullptr, "tools/call", METHOD_TOOLS_CALL),
	METHOD(nullptr, "server/discover", METHOD_DISCOVER),
	METHOD(nullptr, "notification", METHOD_NOTIFY),
	METHOD(nullptr, "metrics", METHOD_METRICS),
	METHOD(nullptr, "other", METHOD_OTHER),
	CALLS("askai_calls_cancelled_total", "Tool calls a loop cancelled while their "
	      "job ran: hang-ups, closed streams, drains (home ends those that "
	      "notifications/cancelled names).", cancelled),
	CALLS("askai_calls_dropped_total", "Job completions a loop dropped, their "
	      "call gone (a stale handle).", dropped),
	CONNS("askai_connections", "Connections a client loop serves.", open, GAUGE),
	CONNS("askai_connections_placed_total", "Connections home placed on a "
	      "client loop.", taken, COUNTER),
	JOBS("askai_jobs_submitted_total", "Jobs (upstream calls) submitted to home.",
	     "", submitted, COUNTER),
	JOBS("askai_jobs_ended_total", "Jobs home ended: completed with the "
	     "upstream's answer or error, cancelled, or rejected (busy, or shutting "
	     "down).", "end=\"completed\"", completed, COUNTER),
	JOBS("askai_jobs_ended_total", nullptr, "end=\"cancelled\"", cancelled,
	     COUNTER),
	JOBS("askai_jobs_ended_total", nullptr, "end=\"rejected\"", rejected, COUNTER),
	JOBS("askai_jobs_in_flight", "Jobs home has admitted and not ended.", "",
	     in_flight, GAUGE),
	VALUE("askai_upstream_wait_seconds", "Time from an upstream request to its "
	      "start in libcurl, its host's lookup included.", "", struct http_metrics,
	      wait, HTTP, HISTOGRAM, NS),
	VALUE("askai_upstream_run_seconds", "Time an upstream request ran in libcurl.",
	      "", struct http_metrics, run, HTTP, HISTOGRAM, NS),
	VALUE("askai_upstream_lookups_total", "Upstream host lookups, by result.",
	      "result=\"ok\"", struct http_metrics, lookups[1], HTTP, COUNTER, ONE),
	VALUE("askai_upstream_lookups_total", nullptr, "result=\"failed\"",
	      struct http_metrics, lookups[0], HTTP, COUNTER, ONE),
	VALUE("askai_blocker_jobs_total", "Jobs the blockers did.", "",
	      struct blocker_metrics, done, BLOCKERS, COUNTER, ONE),
	VALUE("askai_blocker_refused_total", "Jobs a full blocker queue refused.", "",
	      struct blocker_metrics, refused, BLOCKERS, COUNTER, ONE),
	VALUE("askai_log_lines_total", "Lines logged.", "", struct log_metrics, lines,
	      LOGGER, COUNTER, ONE),
	VALUE("askai_log_dropped_total", "Lines lost to a full log ring.", "",
	      struct log_metrics, dropped, LOGGER, COUNTER, ONE),
	VALUE("askai_log_suppressed_total", "Lines a rate limit held back.", "",
	      struct log_metrics, suppressed, LOGGER, COUNTER, ONE),
	VALUE("askai_watchdog_reports_total", "Turns the watchdog found too long.", "",
	      struct brainstem_metrics, reports, BRAINSTEM, COUNTER, ONE),
	VALUE("askai_watchdog_late_wakes_total", "Times the watchdog woke late, and "
	      "vouched for no turn's time before.", "", struct brainstem_metrics, late,
	      BRAINSTEM, COUNTER, ONE),
	VALUE("askai_resident_bytes", "Resident memory.", "", struct brainstem_metrics,
	      rss, BRAINSTEM, GAUGE, ONE),
	VALUE("askai_open_fds", "Descriptors open.", "", struct brainstem_metrics, fds,
	      BRAINSTEM, GAUGE, ONE),
	VALUE("askai_max_fds", "The descriptor limit.", "", struct brainstem_metrics,
	      fd_limit, BRAINSTEM, GAUGE, ONE),
	VALUE("askai_pressure_ratio", "Share of the last 10 s in which some task "
	      "stalled on the resource (PSI some avg10).", "resource=\"cpu\"",
	      struct brainstem_metrics, psi[0], PSI, GAUGE, PERMYRIAD),
	VALUE("askai_pressure_ratio", nullptr, "resource=\"memory\"",
	      struct brainstem_metrics, psi[1], PSI, GAUGE, PERMYRIAD),
	VALUE("askai_pressure_ratio", nullptr, "resource=\"io\"",
	      struct brainstem_metrics, psi[2], PSI, GAUGE, PERMYRIAD),
	VALUE("askai_pressure_stalled_seconds_total", "Time some task stalled on the "
	      "resource (PSI some total).", "resource=\"cpu\"", struct brainstem_metrics,
	      psi_total[0], PSI, COUNTER, US),
	VALUE("askai_pressure_stalled_seconds_total", nullptr, "resource=\"memory\"",
	      struct brainstem_metrics, psi_total[1], PSI, COUNTER, US),
	VALUE("askai_pressure_stalled_seconds_total", nullptr, "resource=\"io\"",
	      struct brainstem_metrics, psi_total[2], PSI, COUNTER, US),
	VALUE("askai_memory_pressure_level", "Memory pressure: 0 none, 1 high, 2 "
	      "critical (design section 5).", "", struct brainstem_metrics, pressure,
	      BRAINSTEM, GAUGE, ONE),
	INPUT("The memory pressure level each input gives.", "resident",
	      PRESSURE_RESIDENT),
	INPUT(nullptr, "system", PRESSURE_SYSTEM),
	INPUT(nullptr, "cgroup", PRESSURE_CGROUP),
	INPUT(nullptr, "psi", PRESSURE_PSI),
	INPUT(nullptr, "test", PRESSURE_TEST),
	VALUE("askai_memory_high_bytes", "Resident memory from which memory pressure "
	      "is high.", "", struct brainstem_metrics, mem_high, BRAINSTEM, GAUGE, ONE),
	VALUE("askai_memory_total_bytes", "The system's memory (MemTotal).", "",
	      struct brainstem_metrics, mem_total, BRAINSTEM, GAUGE, ONE),
	VALUE("askai_memory_available_bytes", "The system's memory available "
	      "(MemAvailable).", "", struct brainstem_metrics, mem_avail, BRAINSTEM,
	      GAUGE, ONE),
	VALUE("askai_cgroup_memory_bytes", "Memory use of the cgroup nearest its "
	      "limit, less its inactive file cache (memory.current, memory.stat).", "",
	      struct brainstem_metrics, cg_usage, CGROUP, GAUGE, ONE),
	VALUE("askai_cgroup_memory_limit_bytes", "That limit, the lower of memory.high "
	      "and memory.max.", "", struct brainstem_metrics, cg_limit, CGROUP, GAUGE,
	      ONE),
	STASHED("askai_stash_retained_bytes", "Buffer storage a loop keeps for reuse.", "",
	        retained, GAUGE, ONE),
	STASHED("askai_stash_takes_total", "Buffer storage taken: from the stash, or "
	        "from the allocator when the stash had none.", "result=\"hit\"", hits,
	        COUNTER, ONE),
	STASHED("askai_stash_takes_total", nullptr, "result=\"miss\"", misses, COUNTER,
	        ONE),
	STASHED("askai_stash_released_bytes_total", "Buffer storage given back to the "
	        "allocator: under memory pressure, or past twice the most its buffers "
	        "held at once.", "", released, COUNTER, ONE),
	KEPT("askai_spare_requests", "Finished requests a loop keeps for reuse.", "",
	     spares),
	KEPT("askai_spare_jobs", "Jobs a loop keeps for reuse.", "", jobs),
	KEPT("askai_kept_bytes", "Storage a loop keeps for reuse besides its stash: "
	     "finished requests, with their answers' text, and pooled JSON tokeners' "
	     "string buffers.", "kind=\"requests\"", bytes[0]),
	KEPT("askai_kept_bytes", nullptr, "kind=\"tokeners\"", bytes[1]),
	STASHED("askai_memory_reliefs_total", "Reliefs of memory pressure a loop began.",
	        "", reliefs, COUNTER, ONE),
	STASHED("askai_memory_trims_total", "Allocator trims a loop ran, the last of a "
	        "relief's, for every loop.", "", trims, COUNTER, ONE),
	STASHED("askai_memory_trim_seconds_total", "The time those took.", "", trim_ns,
	        COUNTER, NS),
};

#undef INPUT
#undef JOBS
#undef CONNS
#undef CALLS
#undef KEPT
#undef STASHED
#undef VALUE
#undef METHOD
#undef WORK
#undef LOOP_HIST

/* Append text, formatted as by printf(); false if out of memory. */
[[gnu::format(printf, 2, 3)]]
static bool
put (struct buf *out,
     char const *fmt,
     ...)
{
	for (size_t room = 256;;) {
		struct span s = buf_reserve(out, room, SIZE_MAX);
		if (!s.len)
			return false;
		va_list ap;
		va_start(ap, fmt);
		int n = vsnprintf((char *)s.ptr, s.len, fmt, ap);
		va_end(ap);
		if (n < 0)
			return false;
		if ((size_t)n < s.len) {
			buf_commit(out, (size_t)n);
			return true;
		}
		room = (size_t)n + 1;
	}
}

/* A histogram's cumulative buckets, to its own resolution, then its sum and
 * count; the count is the buckets' total, which a writer may have moved on
 * from meanwhile. */
static bool
put_hist (struct buf         *out,
          char const         *name,
          char const         *labels,
          struct hist const  *h)
{
	char const *sep = *labels ? "," : "";
	uint64_t n = 0;
	size_t i = 0;
	for (size_t b = 0; b < ARRAY_SIZE(bounds); ++b) {
		for (size_t end = hist_index(bounds[b].ns); i <= end; ++i)
			n += hist_get(&h->bucket[i]);
		if (!put(out, "%s_bucket{%s%sle=\"%s\"} %" PRIu64 "\n", name, labels, sep,
		         bounds[b].le, n))
			return false;
	}
	for (; i < HIST_BUCKETS; ++i)
		n += hist_get(&h->bucket[i]);
	char const *open = *labels ? "{" : "";
	char const *close = *labels ? "}" : "";
	return put(out, "%s_bucket{%s%sle=\"+Inf\"} %" PRIu64 "\n"
	           "%s_sum%s%s%s %.9g\n%s_count%s%s%s %" PRIu64 "\n",
	           name, labels, sep, n,
	           name, open, labels, close, (double)hist_get(&h->sum) / 1e9,
	           name, open, labels, close, n);
}

static bool
put_value (struct buf             *out,
           char const             *name,
           char const             *labels,
           uint16_t                scale,
           _Atomic uint64_t const *p)
{
	uint64_t v = hist_get(p);
	char const *open = *labels ? "{" : "";
	char const *close = *labels ? "}" : "";
	switch (scale) {
	case NS:
		return put(out, "%s%s%s%s %.9f\n", name, open, labels, close,
		           (double)v / 1e9);
	case US:
		return put(out, "%s%s%s%s %.6f\n", name, open, labels, close,
		           (double)v / 1e6);
	case PERMYRIAD:
		return put(out, "%s%s%s%s %.4f\n", name, open, labels, close,
		           (double)v / 1e4);
	default:
		return put(out, "%s%s%s%s %" PRIu64 "\n", name, open, labels, close, v);
	}
}

/* Where loop @p l's series of @p source are, if it has them. */
static void const *
loop_source (struct metrics_loop const *l,
             unsigned                   source)
{
	void const *const at[PER_LOOP] = {
		[LOOP] = l->loop,
		[STASH] = l->stash,
		[KEPT] = l->kept,
		[CALLS] = l->calls,
		[METHODS] = l->methods,
		[CONNS] = l->conns,
	};
	return at[source];
}

/* The metrics are series, from @p *pos on: an entry's for each loop in turn,
 * which only a loop's have more than one of.  A loop that lacks an entry's
 * series, which only home does, is passed over. */
int
metrics_render (struct metrics_src const *src,
                size_t                   *pos,
                struct buf               *out)
{
	void const *const base[SOURCES] = {
		[HTTP] = src->http,
		[BLOCKERS] = src->blockers,
		[JOBS] = src->jobs,
		[BRAINSTEM] = &brainstem_metrics,
		[LOGGER] = &log_metrics,
		[PSI] = hist_get(&brainstem_metrics.psi_ok) ? &brainstem_metrics : nullptr,
		[CGROUP] = hist_get(&brainstem_metrics.cg_limit) ? &brainstem_metrics : nullptr,
	};
	size_t const n = src->n_loops;
	size_t const end = ARRAY_SIZE(table) * n;
	for (unsigned cost = 0; *pos < end && cost < BUDGET; ) {
		struct entry const *e = &table[*pos / n];
		size_t l = *pos % n;
		bool looped = e->source < PER_LOOP;
		char const *at = looped ? loop_source(&src->loops[l], e->source)
		                        : base[e->source];
		/* An entry that is no loop's has a series once. */
		*pos = looped ? *pos + 1 : (*pos / n + 1) * n;
		if (!at)
			continue;
		at += e->off;
		/* A loop's family starts with the first loop, which has them all. */
		char labels[128];
		if (looped)
			snprintf(labels, sizeof labels, "loop=\"%s\"%s%s", src->loops[l].name,
			         *e->labels ? "," : "", e->labels);
		if (e->help && !l && !put(out, "# HELP %s %s\n# TYPE %s %s\n", e->name,
		                          e->help, e->name, kinds[e->kind]))
			return ENOMEM;
		char const *ls = looped ? labels : e->labels;
		bool ok = e->kind == HISTOGRAM
		        ? put_hist(out, e->name, ls, (struct hist const *)(void const *)at)
		        : put_value(out, e->name, ls, e->scale,
		                    (_Atomic uint64_t const *)(void const *)at);
		if (!ok)
			return ENOMEM;
		cost += e->kind == HISTOGRAM ? 8 : 1;
	}
	return *pos < end ? EAGAIN : 0;
}
