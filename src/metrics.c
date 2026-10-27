/** @file Prometheus text exposition of the core's metrics (metrics.h). */
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>

#include "blocker.h"
#include "brainstem.h"
#include "buf.h"
#include "home.h"
#include "http.h"
#include "log.h"
#include "loop.h"
#include "ledger.h"
#include "manual.h"
#include "metrics.h"
#include "task.h"
#include "util.h"

/* What a call renders at most, a histogram costing 8 and a value 1: a
 * histogram's buckets take a pass over all of its own. */
enum { BUDGET = 32 };

/* Where a series is: those before PER_LOOP are each loop's, rendered for
 * every loop that has them, labelled with its name. */
enum source {
	LOOP, STASH, KEPT, CALLS, METHODS, CONNS, TASKS, PER_LOOP,
	HTTP = PER_LOOP, BLOCKERS, JOBS, MAN, TASKS_ALL, LEDGER, BRAINSTEM, LOGGER, PSI,
	CGROUP,
	SOURCES
};
enum kind { COUNTER, GAUGE, HISTOGRAM };
enum scale { ONE, NANO, MICRO, PERMYRIAD };

/* A series, or a loop's; help, given for the first of a family only, starts
 * the family. */
struct metrics_entry {
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
	{ name, help, labels, offsetof(struct loop_metrics, field), LOOP, HISTOGRAM, NANO }
#define WORK(help, unit, u) \
	{ "askai_loop_step_work_max", help, "unit=\"" unit "\"", \
	  offsetof(struct loop_metrics, work[u]), LOOP, GAUGE, ONE }
#define METHOD(help, method, m) \
	{ "askai_request_seconds", help, "method=\"" method "\"", \
	  (m) * sizeof (struct hist), METHODS, HISTOGRAM, NANO }
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
#define MAN(name, help, labels, field, kind) \
	VALUE(name, help, labels, struct manual_metrics, field, MAN, kind, ONE)
#define TASKS(name, help, field, kind) \
	VALUE(name, help, "", struct task_metrics, field, TASKS, kind, ONE)
#define TASKS_ALL(name, help, labels, field) \
	VALUE(name, help, labels, struct task_pool_metrics, field, TASKS_ALL, COUNTER, ONE)
#define LEDGERS(name, help, labels, field, kind) \
	VALUE(name, help, labels, struct ledger_metrics, field, LEDGER, kind, ONE)
#define INPUT(help, input, i) \
	VALUE("askai_memory_pressure_input_level", help, "input=\"" input "\"", \
	      struct brainstem_metrics, inputs[i], BRAINSTEM, GAUGE, ONE)

static_assert(LEDGER_RUNNING == 8, "the help of askai_mcp_jobs_refused_total says");

static struct metrics_entry const table[] = {
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
	WORK(nullptr, "tasks", LOOP_TASKS),
	WORK(nullptr, "flights", LOOP_FLIGHTS),
	METHOD("Time from a request's head to its end, by MCP method.", "initialize",
	       METHOD_INITIALIZE),
	METHOD(nullptr, "ping", METHOD_PING),
	METHOD(nullptr, "tools/list", METHOD_TOOLS_LIST),
	METHOD(nullptr, "tools/call", METHOD_TOOLS_CALL),
	METHOD(nullptr, "server/discover", METHOD_DISCOVER),
	METHOD(nullptr, "tasks/get", METHOD_TASKS_GET),
	METHOD(nullptr, "tasks/update", METHOD_TASKS_UPDATE),
	METHOD(nullptr, "tasks/cancel", METHOD_TASKS_CANCEL),
	METHOD(nullptr, "notification", METHOD_NOTIFY),
	METHOD(nullptr, "metrics", METHOD_METRICS),
	METHOD(nullptr, "other", METHOD_OTHER),
	CALLS("askai_calls_cancelled_total", "Tool calls that a loop cancelled while "
	      "their job ran, for a hang-up, a closed stream, a drain or a "
	      "notifications/cancelled.", cancelled),
	CALLS("askai_calls_dropped_total", "Job completions a loop dropped, their "
	      "call gone (a stale handle).", dropped),
	CALLS("askai_turns_total", "Upstream calls made as turns of ask calls that "
	      "lend the upstream tools (--upstream-tools).", turns),
	VALUE("askai_turn_tool_calls_total", "Tool calls the turns made, by tool; "
	      "unknown: one not lent, or of no tool's name.", "tool=\"man\"",
	      struct metrics_calls, lent[LENT_MAN], CALLS, COUNTER, ONE),
	VALUE("askai_turn_tool_calls_total", nullptr, "tool=\"git\"",
	      struct metrics_calls, lent[LENT_GIT], CALLS, COUNTER, ONE),
	VALUE("askai_turn_tool_calls_total", nullptr, "tool=\"notes\"",
	      struct metrics_calls, lent[LENT_NOTES], CALLS, COUNTER, ONE),
	VALUE("askai_turn_tool_calls_total", nullptr, "tool=\"compact\"",
	      struct metrics_calls, lent[LENT_COMPACT], CALLS, COUNTER, ONE),
	VALUE("askai_turn_tool_calls_total", nullptr, "tool=\"background\"",
	      struct metrics_calls, lent[LENT_BACKGROUND], CALLS, COUNTER, ONE),
	VALUE("askai_turn_tool_calls_total", nullptr, "tool=\"jobs\"",
	      struct metrics_calls, lent[LENT_JOBS], CALLS, COUNTER, ONE),
	VALUE("askai_turn_tool_calls_total", nullptr, "tool=\"wait\"",
	      struct metrics_calls, lent[LENT_WAIT], CALLS, COUNTER, ONE),
	VALUE("askai_turn_tool_calls_total", nullptr, "tool=\"timer\"",
	      struct metrics_calls, lent[LENT_TIMER], CALLS, COUNTER, ONE),
	VALUE("askai_turn_tool_calls_total", nullptr, "tool=\"plugin\"",
	      struct metrics_calls, lent[LENT_PLUGIN], CALLS, COUNTER, ONE),
	VALUE("askai_turn_tool_calls_total", nullptr, "tool=\"unknown\"",
	      struct metrics_calls, lent[LENT_NONE], CALLS, COUNTER, ONE),
	VALUE("askai_compactions_total", "Contexts of ask calls' turns compacted: "
	      "by the upstream's compact call, or by askai, the context full.",
	      "by=\"upstream\"", struct metrics_calls, compacted[0], CALLS, COUNTER,
	      ONE),
	VALUE("askai_compactions_total", nullptr, "by=\"askai\"", struct metrics_calls,
	      compacted[1], CALLS, COUNTER, ONE),
	CALLS("askai_turns_stopped_total", "Ask calls stopped as their turns ran out "
	      "(--max-turns), tools still called.", stopped),
	CONNS("askai_connections", "Connections a client loop serves.", open, GAUGE),
	CONNS("askai_connections_placed_total", "Connections home placed on a "
	      "client loop.", taken, COUNTER),
	VALUE("askai_tls_handshakes_total", "TLS handshakes a client loop did, by "
	      "result: done, or begun and failed or cut off.", "result=\"done\"",
	      struct metrics_conns, shaken, CONNS, COUNTER, ONE),
	VALUE("askai_tls_handshakes_total", nullptr, "result=\"failed\"",
	      struct metrics_conns, unshaken, CONNS, COUNTER, ONE),
	VALUE("askai_tls_flight_seconds", "Time a TLS handshake's flight took on a "
	      "client loop: its CPU, mostly.", "", struct metrics_conns, flights,
	      CONNS, HISTOGRAM, NANO),
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
	JOBS("askai_jobs_queued", "Jobs that wait for an upstream slot in their "
	     "principals' queues.", "", queued, GAUGE),
	JOBS("askai_principals_queued", "Principals, sessions or else client "
	     "addresses, with jobs that wait for an upstream slot.", "", principals,
	     GAUGE),
	JOBS("askai_jobs_dispatched_total", "Jobs started upstream, a principal's at "
	     "a time, round-robin.", "", dispatched, COUNTER),
	JOBS("askai_upstream_calls", "Upstream calls under way.", "", running, GAUGE),
	JOBS("askai_upstream_slots", "Upstream calls at once at most: --slots, "
	     "llama-server's /props, or 4.", "", slots, GAUGE),
	JOBS("askai_upstream_cancels_total", "Upstream calls cut once their request "
	     "had gone: of cancelled jobs, or preempted ones.", "", aborted, COUNTER),
	JOBS("askai_upstream_chunks_total", "Events of streamed answers.", "", chunks,
	     COUNTER),
	JOBS("askai_upstream_stalls_total", "Answers ended by the chunk timeout.", "",
	     stalls, COUNTER),
	JOBS("askai_breaker_state", "The upstream breaker: 0 closed, 1 open, 2 "
	     "half-open.", "", breaker, GAUGE),
	JOBS("askai_breaker_opens_total", "Times the upstream breaker opened.", "",
	     opens, COUNTER),
	JOBS("askai_upstream_background_calls", "Upstream calls of background jobs "
	     "under way.", "", background, GAUGE),
	JOBS("askai_jobs_preempted_total", "Background jobs whose upstream call an "
	     "interactive job preempted, to start again later.", "", preempted, COUNTER),
	JOBS("askai_breaker_probes_total", "Jobs the half-open breaker let through "
	     "as probes.", "", probes, COUNTER),
	MAN("askai_man_pages", "Pages the man tool's manual keeps.", "", pages, GAUGE),
	MAN("askai_man_page_bytes", "Their text.", "", bytes, GAUGE),
	MAN("askai_man_asks_total", "Pages man calls asked for: a page kept and "
	    "current answered, or the call waited for man.", "result=\"hit\"", hits,
	    COUNTER),
	MAN("askai_man_asks_total", nullptr, "result=\"wait\"", waits, COUNTER),
	MAN("askai_man_runs_total", "Runs of man: to find a page's file (man -w), "
	    "or to render the page (man -l).", "run=\"find\"", finds, COUNTER),
	MAN("askai_man_runs_total", nullptr, "run=\"render\"", renders, COUNTER),
	TASKS("askai_tasks_held", "Tasks a client loop holds.", held, GAUGE),
	TASKS("askai_task_slices_total", "Task slices a client loop ran.", slices,
	      COUNTER),
	TASKS("askai_tasks_moved_total", "Tasks a client loop moved to another.",
	      moved, COUNTER),
	TASKS_ALL("askai_tasks_submitted_total", "Tasks submitted.", "", submitted),
	TASKS_ALL("askai_tasks_ended_total", "Tasks ended: their slices done, found "
	          "cancelled, or held by a loop that stopped.", "end=\"done\"", done),
	TASKS_ALL("askai_tasks_ended_total", nullptr, "end=\"cancelled\"", cancelled),
	TASKS_ALL("askai_tasks_ended_total", nullptr, "end=\"abandoned\"", abandoned),
	LEDGERS("askai_mcp_tasks_opened_total", "Tool calls that became tasks of MCP's "
	        "tasks extension.", "", opened, COUNTER),
	LEDGERS("askai_mcp_jobs_opened_total", "Calls of background of clients that "
	        "started jobs.", "", jobs, COUNTER),
	LEDGERS("askai_mcp_jobs_refused_total", "Calls of background of clients that "
	        "started no job, because 8 jobs and tasks of the owner ran.", "",
	        refused, COUNTER),
	LEDGERS("askai_mcp_timers_set_total", "Timers that clients set.", "", timers,
	        COUNTER),
	LEDGERS("askai_mcp_waits", "Calls of wait of clients that wait now.", "", waits,
	        GAUGE),
	LEDGERS("askai_mcp_tasks_ended_total", "Tasks and jobs whose calls ended, by "
	        "status.", "status=\"completed\"", ended[LEDGER_COMPLETED], COUNTER),
	LEDGERS("askai_mcp_tasks_ended_total", nullptr, "status=\"failed\"",
	        ended[LEDGER_FAILED], COUNTER),
	LEDGERS("askai_mcp_tasks_ended_total", nullptr, "status=\"cancelled\"",
	        ended[LEDGER_CANCELLED], COUNTER),
	LEDGERS("askai_mcp_tasks_gone_total", "Records of tasks and jobs removed, an "
	        "hour after their calls ended or to make room.", "", gone, COUNTER),
	LEDGERS("askai_mcp_tasks_full_total", "Tool calls that did not become tasks or "
	        "jobs, because the calls of all records in the ledger ran.", "", full,
	        COUNTER),
	LEDGERS("askai_mcp_tasks_kept", "Records of tasks and jobs in the ledger.", "",
	        kept, GAUGE),
	LEDGERS("askai_mcp_task_result_bytes", "Their results.", "", bytes, GAUGE),
	VALUE("askai_upstream_wait_seconds", "Time from an upstream request to its "
	      "start in libcurl, its host's lookup included.", "", struct http_metrics,
	      wait, HTTP, HISTOGRAM, NANO),
	VALUE("askai_upstream_run_seconds", "Time an upstream request ran in libcurl.",
	      "", struct http_metrics, run, HTTP, HISTOGRAM, NANO),
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
	      psi_total[0], PSI, COUNTER, MICRO),
	VALUE("askai_pressure_stalled_seconds_total", nullptr, "resource=\"memory\"",
	      struct brainstem_metrics, psi_total[1], PSI, COUNTER, MICRO),
	VALUE("askai_pressure_stalled_seconds_total", nullptr, "resource=\"io\"",
	      struct brainstem_metrics, psi_total[2], PSI, COUNTER, MICRO),
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
	        COUNTER, NANO),
};

#undef INPUT
#undef TASKS_ALL
#undef TASKS
#undef MAN
#undef JOBS
#undef CONNS
#undef CALLS
#undef KEPT
#undef STASHED
#undef VALUE
#undef METHOD
#undef WORK
#undef LOOP_HIST

/* A histogram's cumulative buckets, to its own resolution, then its sum and
 * count; the count is the buckets' total, which a writer may have moved on
 * from meanwhile. */
static int
metrics_put_hist (struct buf         *out,
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
		int e = buf_printf(out, "%s_bucket{%s%sle=\"%s\"} %" PRIu64 "\n", name, labels,
		                   sep, bounds[b].le, n);
		if (e)
			return e;
	}
	for (; i < HIST_BUCKETS; ++i)
		n += hist_get(&h->bucket[i]);
	char const *open = *labels ? "{" : "";
	char const *close = *labels ? "}" : "";
	return buf_printf(out, "%s_bucket{%s%sle=\"+Inf\"} %" PRIu64 "\n"
	                  "%s_sum%s%s%s %.9g\n%s_count%s%s%s %" PRIu64 "\n",
	                  name, labels, sep, n,
	                  name, open, labels, close, (double)hist_get(&h->sum) / 1e9,
	                  name, open, labels, close, n);
}

static int
metrics_put_value (struct buf             *out,
                   char const             *name,
                   char const             *labels,
                   uint16_t                scale,
                   _Atomic uint64_t const *p)
{
	uint64_t v = hist_get(p);
	char const *open = *labels ? "{" : "";
	char const *close = *labels ? "}" : "";
	switch (scale) {
	case NANO:
		return buf_printf(out, "%s%s%s%s %.9f\n", name, open, labels, close,
		                  (double)v / 1e9);
	case MICRO:
		return buf_printf(out, "%s%s%s%s %.6f\n", name, open, labels, close,
		                  (double)v / 1e6);
	case PERMYRIAD:
		return buf_printf(out, "%s%s%s%s %.4f\n", name, open, labels, close,
		                  (double)v / 1e4);
	default:
		return buf_printf(out, "%s%s%s%s %" PRIu64 "\n", name, open, labels, close,
		                  v);
	}
}

/* Where loop @p l's series of @p source are, if it has them. */
static void const *
metrics_loop_source (struct metrics_loop const *l,
                     unsigned                   source)
{
	void const *const at[PER_LOOP] = {
		[LOOP] = l->loop,
		[STASH] = l->stash,
		[KEPT] = l->kept,
		[CALLS] = l->calls,
		[METHODS] = l->methods,
		[CONNS] = l->conns,
		[TASKS] = l->tasks,
	};
	return at[source];
}

/* Entry @p m's series at @p at, labelled @p labels, its family's help and
 * type first if @p first, counted into @p *cost. */
static int
metrics_put_series (struct buf                 *out,
                    struct metrics_entry const *m,
                    char const                 *labels,
                    void const                 *at,
                    bool                        first,
                    unsigned                   *cost)
{
	if (m->help && first) {
		int e = buf_printf(out, "# HELP %s %s\n# TYPE %s %s\n", m->name, m->help,
		                   m->name, kinds[m->kind]);
		if (e)
			return e;
	}
	if (m->kind == HISTOGRAM) {
		*cost += 8;
		return metrics_put_hist(out, m->name, labels, at);
	}
	*cost += 1;
	return metrics_put_value(out, m->name, labels, m->scale, at);
}

/* Loop @p loop's series of entry @p m, if it has them: labelled with its
 * name, and @p m's labels, if any.  A loop's family starts with the first
 * loop, which has them all. */
static int
metrics_put_loop (struct buf                 *out,
                  struct metrics_entry const *m,
                  struct metrics_loop const  *loop,
                  bool                        first,
                  unsigned                   *cost)
{
	char const *at = metrics_loop_source(loop, m->source);
	if (!at)
		return 0;
	char labels[128];
	int n = *m->labels ? snprintf(labels, sizeof labels, "loop=\"%s\",%s", loop->name,
	                              m->labels)
	                   : snprintf(labels, sizeof labels, "loop=\"%s\"", loop->name);
	if (n < 0 || n >= (int)sizeof labels)
		return EOVERFLOW;
	return metrics_put_series(out, m, labels, at + m->off, first, cost);
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
		[MAN] = src->man,
		[TASKS_ALL] = src->tasks,
		[LEDGER] = src->ledger,
		[BRAINSTEM] = &brainstem_metrics,
		[LOGGER] = &log_metrics,
		[PSI] = hist_get(&brainstem_metrics.psi_ok) ? &brainstem_metrics : nullptr,
		[CGROUP] = hist_get(&brainstem_metrics.cg_limit) ? &brainstem_metrics : nullptr,
	};
	size_t const n = src->n_loops;
	size_t const end = ARRAY_SIZE(table) * n;
	for (unsigned cost = 0; *pos < end && cost < BUDGET; ) {
		struct metrics_entry const *m = &table[*pos / n];
		size_t l = *pos % n;
		int e = 0;
		if (m->source < PER_LOOP) {
			++*pos;
			e = metrics_put_loop(out, m, &src->loops[l], !l, &cost);
		} else {
			/* An entry that is no loop's has a series once, from its first
			 * position on. */
			char const *at = base[m->source];
			*pos = (*pos / n + 1) * n;
			e = at ? metrics_put_series(out, m, m->labels, at + m->off, true, &cost) : 0;
		}
		if (e)
			return e;
	}
	return *pos < end ? EAGAIN : 0;
}
