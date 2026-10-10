#!/usr/bin/bash
# Run a named load-harness profile against askai and an upstream mock, and
# judge it.
#
# Usage: run.sh PROFILE [ASKAI_BIN [LOAD_DIR [DURATION]]]
#   PROFILE    baseline | idle | jobs | answers | sse | loris | slowread |
#              pipeline | hostile | dense | malformed | storm | check | stall
#   ASKAI_BIN  the askai executable to test (default: $ASKAI or ./askai)
#   LOAD_DIR   directory holding the built gen and mock (default: this dir)
#   DURATION   seconds of load (default: 10)
#
# Env: RATE, the probe's Hz (default 1000); BASE, the seconds of the idle
# phase (default 3); AFTER_S and LEAK_S, the least and most seconds of the
# phase after the load (defaults DURATION, so that the two phases weigh
# alike, and 10); ASKAI_CPUS and LOAD_CPUS,
# taskset(1) CPU lists for askai and for the generator and mock; STATS, what
# askai reports on SIGUSR1: full (the default), loop (S2a: no work line) or
# none (S0, S1: none, and the signal would end it), the clauses it cannot
# report then being n/a; and the verdict's factors below.
#
# A run has up to four phases on one askai: the probe alone for BASE seconds,
# which measures this machine as it is now; for a profile with heavy modes
# (pipeline, malformed, hostile, dense), those alone, a few rounds each, which
# time their batches and bodies on it (SOLO lines); the profile's load with the
# probe for DURATION seconds; and the probe alone again while the load's
# connections go and askai gives their memory back, which it does in one call
# once quiet, for AFTER_S seconds at least and until its RSS is back near the
# idle phase's, LEAK_S at most.  It prints the generator's lines (the idle and
# after phases' prefixed IDLE and AFTER: SUMMARY, CONTROL, PROBE, FAIR, MOCK
# and BG), askai's loop statistics for each phase (LOOP idle, load and after:
# turn and step times, timer lateness, and the most work of each unit a step
# did, which SIGUSR1 makes askai print), its RSS and descriptors (RESOURCES:
# idle, peak, and after the last phase), the mock's final counters, latency
# RATIOs of the load and after phases against the idle phase, a CHECK line per
# clause of the verdict, and the VERDICT.
#
# The verdict follows the latency policy of doc/core-design.md section 8: the
# daemon runs on whatever machine it is given, busy or slow, and its clients
# should share in that, so no absolute latency fails a run.  It fails on:
#  - errors: probe arrivals that failed, timed out or were lost, in any phase;
#    background requests left unanswered (idle, and the malformed, hostile and
#    dense requests sent whole; dense ones not answered 200); tool calls that
#    failed or were cut off; mock counters that do not add up;
#  - lockout: a fresh connection not served (fresh-ping, churn);
#  - limits: loris or slow-body clients still open once their deadline is
#    past; an upstream call that outlived its client, 3 s after the load (in
#    the sse profile, whose clients all hang up on their event streams, any
#    call answered rather than cancelled);
#  - leaks: descriptors not back at the idle count + 2 (libcurl may keep
#    upstream connections), or RSS not back within RSS_SLACK_KIB (default
#    4096) of the idle phase, by the end of the after phase.  ASan's
#    allocator keeps what is freed: against a sanitized askai set
#    RSS_SLACK_KIB high and let LeakSanitizer, at askai's exit, judge leaks;
#  - unfairness: a probe connection's p90 over FAIR_CONN times (default 10)
#    the median connection's, each connection having taken its turn of the
#    arrivals; a probe kind's p90 over FAIR_KIND times (default 10) the
#    median kind's; a tool-call connection that completed no call while the
#    median one completed 3; a heavy mode whose p90 batch or body took over
#    FAIR_HEAVY times (default 50) its p90 alone.  A client held back is slow
#    throughout, so the p90 shows it, while the machine's own hiccups, which
#    hit every client alike, only reach a client's p99 when a few happen to
#    land on it; the p99 spreads are printed too.  Heavy work yields to cheap
#    requests by design, so its factor is the larger;
#  - unbounded work: a unit of work done in a step over its bound (loop.h),
#    in any phase; units count work that cannot be split in full;
#  - gross outliers against the idle phase: the probe's p99 in the load or
#    the after phase over RTT_GROSS times (default 100) the idle p99, or a
#    turn in either over TURN_GROSS times (default 1000) the idle phase's p99
#    turn and over RTT_GROSS times its probe p99, for a machine that preempts
#    askai stretches every turn and every reply alike.
# The "check" profile is a light mix of every mode, short deadlines and a few
# thousand connections for `make check-load`.  The "stall" profile instead
# runs the coordinated-omission self-check: it
# freezes askai with SIGSTOP for 320 arrivals' worth of time (10 x the 32
# keep-alive probe connections) and checks that the schedule-timed probe sees
# most frozen arrivals take over a quarter of the freeze while the
# dispatch-timed control of the same run does not.
#
# Exit status: 0 if the verdict passes; 1 if askai did not exit 0 on SIGTERM
# (e.g. it crashed) or the harness failed (the generator lost arrivals, or the
# mock did not exit 0); 3 if the verdict fails.

set -u
self=${0##*/}
here=$(cd "${0%/*}" && pwd)

profile=${1:-storm}
askai=${2:-${ASKAI:-./askai}}
dir=${3:-$here}
dur=${4:-10}
rate=${RATE:-1000}
base=${BASE:-3}
gen=$dir/gen
mock=$dir/mock

for bin in "$gen" "$mock" "$askai"; do
	if [[ ! -x $bin ]]; then
		printf '%s: not executable: %s\n' "$self" "$bin" >&2
		exit 2
	fi
done

# Seconds of load after which loris and slow-body clients must be closed, and
# the conntrack entries a run needs free (see below), of the table's ct_max.
stall=0 askai_args=() hold_s=10 ct_free='ct_max / 2'
case $profile in
baseline)  mock_args=(-s 4 -d 5 -z 2);   gen_args=() ;;
idle)      mock_args=(-s 4 -d 5 -z 2);   gen_args=(--idle 5000) ;;
jobs)      mock_args=(-s 1 -d 50 -z 16); gen_args=(--jobs 64) ;;
answers)   mock_args=(-s 2 -d 5 -z 262144); gen_args=(--jobs 16) ;;
sse)       mock_args=(-s 4 -d 60000);    gen_args=(--hangup 64) ;;
loris)     mock_args=(-s 4 -d 5);        gen_args=(--loris 128 --slowbody 128) ;;
slowread)  mock_args=(-s 4 -d 5);        gen_args=(--slowread 128) ;;
pipeline)  mock_args=(-s 4 -d 5);        gen_args=(--pipeline 4) ;;
hostile)   mock_args=(-s 4 -d 5);        gen_args=(--hostile) ;;
dense)     mock_args=(-s 4 -d 5);        gen_args=(--dense)
           askai_args=(--max-values 1000000) ;;
malformed) mock_args=(-s 4 -d 5);        gen_args=(--malformed --churn 1000)
           ct_free='ct_max * 2 / 3' ;;
stall)     mock_args=(-s 4 -d 5 -z 2);   gen_args=(); stall=1 ;;
storm)     mock_args=(-s 1 -d 20 -z 64)
           gen_args=(--idle 10000 --jobs 64 --loris 64 --slowbody 64
                     --slowread 64 --pipeline 4 --churn 1000 --malformed
                     --hostile)
           ct_free='ct_max * 2 / 3' ;;
check)     mock_args=(-s 2 -d 50 -z 131072)
           gen_args=(--idle 500 --jobs 8 --hangup 4 --loris 16 --slowbody 16
                     --slowread 8 --pipeline 1 --churn 200 --hostile --dense)
           askai_args=(--max-values 1000000 --head-timeout 2 --body-grace 1)
           hold_s=3 ct_free=20000 ;;
*) printf '%s: unknown profile: %s\n' "$self" "$profile" >&2; exit 2 ;;
esac

# Netfilter, where loaded, tracks every connection, closed ones for 120 s,
# and a storm makes up to 140 000: back to back, storms fill its table, and
# new connections then stall on dropped SYNs.  Start only with room for a run:
# two thirds of the table free for the profiles that open the most
# connections (half left too little for a storm that followed another).
ct=/proc/sys/net/netfilter/nf_conntrack_count
if [[ -r $ct ]]; then
	ct_max=$(< /proc/sys/net/netfilter/nf_conntrack_max)
	ct_top=$((ct_max - (ct_free)))
	for ((i = 0; i < 180 && $(< "$ct") > ct_top; ++i)); do
		((i)) || printf '%s: waiting for the conntrack table to drain\n' \
		                "$self" >&2
		sleep 1
	done
fi

pin_askai=() pin_load=()
[[ -n ${ASKAI_CPUS-} ]] && pin_askai=(taskset -c "$ASKAI_CPUS")
[[ -n ${LOAD_CPUS-} ]] && pin_load=(taskset -c "$LOAD_CPUS")

tmp=$(mktemp -d)
mock_pid= askai_pid= stall_pid= sample_pid= after_pid=
cleanup () {
	[[ -n $sample_pid ]] && kill "$sample_pid" 2> /dev/null
	[[ -n $stall_pid ]] && kill "$stall_pid" 2> /dev/null
	[[ -n $after_pid ]] && kill "$after_pid" 2> /dev/null
	[[ -n $askai_pid ]] && kill -TERM "$askai_pid" 2> /dev/null
	[[ -n $mock_pid  ]] && kill -TERM "$mock_pid" 2> /dev/null
	wait 2> /dev/null
	rm -rf "$tmp"
}
trap cleanup EXIT INT TERM

# Print askai's resident set (KiB) and descriptor count.
resources () {
	local rss
	rss=$(awk '/^VmRSS:/ { print $2 }' "/proc/$askai_pid/status" 2> /dev/null) &&
	printf '%s %s\n' "$rss" "$(ls "/proc/$askai_pid/fd" 2> /dev/null | wc -l)"
}

# Have askai print its loop statistics since the previous SIGUSR1, wait up to
# 1 s for them in its log, and print them as LOOP lines labelled PHASE, if
# given.
stats=${STATS:-full} dumps=0
loop_stats () {
	local phase=${1-} i
	[[ $stats == none ]] && return 0
	kill -USR1 "$askai_pid" 2> /dev/null || return 1
	((++dumps))
	for ((i = 0; i < 100; ++i)); do
		(($(grep -c '^askai: stats timer_late ' "$tmp/askai.err") >= dumps)) &&
			break
		sleep 0.01
	done
	((i < 100)) || return 1
	sleep 0.05
	[[ -z $phase ]] || awk '/^askai: stats turn / { last = "" }
	                       /^askai: stats / { last = last $0 "\n" }
	                       END { printf "%s", last }' "$tmp/askai.err" |
		sed "s/^askai: stats /LOOP $phase /"
}

# Wait up to 5 s for a "127.0.0.1:PORT" line in FILE; echo the first PORT.
wait_port () {
	local file=$1 i port
	for ((i = 0; i < 100; ++i)); do
		port=$(grep -oE '127\.0\.0\.1:[0-9]+' "$file" 2> /dev/null |
		       head -1 | cut -d: -f2)
		[[ -n $port ]] && { echo "$port"; return 0; }
		sleep 0.05
	done
	return 1
}

"${pin_load[@]}" "$mock" -l 127.0.0.1:0 "${mock_args[@]}" 2> "$tmp/mock.err" &
mock_pid=$!
mock_port=$(wait_port "$tmp/mock.err") || {
	echo "$self: mock did not start" >&2; cat "$tmp/mock.err" >&2; exit 1; }

"${pin_askai[@]}" "$askai" -l 127.0.0.1:0 "${askai_args[@]}" \
	-u "http://127.0.0.1:$mock_port/v1/chat/completions" -m m \
	2> "$tmp/askai.err" &
askai_pid=$!
askai_port=$(wait_port "$tmp/askai.err") || {
	echo "$self: askai did not start" >&2; cat "$tmp/askai.err" >&2; exit 1; }

printf '=== profile %s: rate=%s/s base=%ss duration=%ss askai_port=%s ' \
       "$profile" "$rate" "$base" "$dur" "$askai_port"
printf 'mock_port=%s askai_args=%s ===\n' "$mock_port" "${askai_args[*]:-}"
idle_res=$(resources)
loop_stats

# The idle phase: the probe alone.
"${pin_load[@]}" "$gen" -H 127.0.0.1 -p "$askai_port" -r "$rate" -d "$base" \
	2> "$tmp/idle.err" | sed 's/^/IDLE /' | tee "$tmp/gen.out" |
	grep -E '^IDLE (SUMMARY kind=all|CONTROL|FAIR)'
idle_rc=${PIPESTATUS[0]}
loop_stats idle | tee -a "$tmp/gen.out"

# The heavy modes alone, for what their bodies and batches take on this
# machine now.
solo_args=()
for ((i = 0; i < ${#gen_args[@]}; ++i)); do
	case ${gen_args[i]} in
	--malformed|--hostile|--dense) solo_args+=("${gen_args[i]}") ;;
	--pipeline) solo_args+=("${gen_args[i]}" "${gen_args[i + 1]}") ;;
	esac
done
solo_rc=0
if ((${#solo_args[@]})); then
	"${pin_load[@]}" "$gen" -H 127.0.0.1 -p "$askai_port" --solo "${solo_args[@]}" \
		2> "$tmp/solo.err" | tee -a "$tmp/gen.out"
	solo_rc=${PIPESTATUS[0]}
	loop_stats
fi

if ((stall)); then
	stall_s=$(awk -v r="$rate" 'BEGIN { printf "%.3f", 320 / r }')
	gen_args+=(--slow "$(awk -v s="$stall_s" 'BEGIN { printf "%d", s * 250000 }')")
	( sleep "$(awk -v d="$dur" 'BEGIN { print d / 2 }')"
	  kill -STOP "$askai_pid" 2> /dev/null
	  sleep "$stall_s"
	  kill -CONT "$askai_pid" 2> /dev/null ) &
	stall_pid=$!
fi

# The load.
while resources; do sleep 0.5; done > "$tmp/res" 2> /dev/null &
sample_pid=$!
"${pin_load[@]}" "$gen" -H 127.0.0.1 -p "$askai_port" -r "$rate" -d "$dur" \
	--stats "$mock_port" "${gen_args[@]}" 2> "$tmp/gen.err" | tee -a "$tmp/gen.out"
gen_rc=${PIPESTATUS[0]}
kill "$sample_pid" 2> /dev/null
sample_pid=
cat "$tmp/idle.err" "$tmp/gen.err"
loop_stats load | tee -a "$tmp/gen.out"

# The after phase: the probe alone again, while the load's connections go and
# askai gives their memory back, until its RSS is back near the idle phase's;
# then the probe's own connections go too, and its descriptors must follow.
after_s=${AFTER_S:-$dur} leak_s=${LEAK_S:-10}
((leak_s > after_s)) || leak_s=$after_s
read -r rss0 fds0 <<< "${idle_res:-0 0}"
"${pin_load[@]}" "$gen" -H 127.0.0.1 -p "$askai_port" -r "$rate" -d "$leak_s" \
	> "$tmp/after.out" 2> "$tmp/after.err" &
after_pid=$!
for ((i = 1; i <= leak_s * 5; ++i)); do
	sleep 0.2
	read -r rss fds <<< "$(resources)" || break
	((i >= after_s * 5 && rss <= rss0 + ${RSS_SLACK_KIB:-4096})) && break
done
kill -INT "$after_pid" 2> /dev/null
wait "$after_pid"
after_rc=$?
after_pid=
sed 's/^/AFTER /' "$tmp/after.out" | tee -a "$tmp/gen.out" |
	grep -E '^AFTER (SUMMARY kind=all|CONTROL|FAIR)'
cat "$tmp/after.err"
loop_stats after | tee -a "$tmp/gen.out"
after=
for ((i = 0; i < 25; ++i)); do
	after=$(resources) || break
	read -r rss fds <<< "$after"
	((fds <= fds0 + 2)) && break
	sleep 0.2
done
awk -v idle="${idle_res:-0 0}" -v after="${after:-0 0}" '
{ if ($1 > rss) rss = $1; if ($2 > fds) fds = $2 }
END {
	split(idle, i, " ")
	split(after, a, " ")
	printf "RESOURCES askai rss_kib idle=%d peak=%d after=%d fds idle=%d " \
	       "peak=%d after=%d\n", i[1], rss, a[1], i[2], fds, a[2]
}' "$tmp/res" | tee -a "$tmp/gen.out"

# Drain askai's upstream work, then read the mock's final counters.
kill -TERM "$askai_pid" 2> /dev/null
wait "$askai_pid" 2> /dev/null
askai_rc=$?
askai_pid=
kill -TERM "$mock_pid" 2> /dev/null
wait "$mock_pid" 2> /dev/null
mock_rc=$?
mock_pid=
grep -E '^mock: (requests|http)' "$tmp/mock.err" | tee -a "$tmp/gen.out"

printf 'PROFILE %s askai_exit=%s gen_exit=%s/%s/%s/%s mock_exit=%s\n' "$profile" \
       "$askai_rc" "$idle_rc" "$solo_rc" "$gen_rc" "$after_rc" "$mock_rc"
awk -v stall="$stall" -v r="$rate" -v s="${stall_s:-0}" -v dur="$dur" \
	-v hold_s="$hold_s" -v stats="$stats" \
	-v fair_conn="${FAIR_CONN:-10}" -v fair_kind="${FAIR_KIND:-10}" \
	-v fair_heavy="${FAIR_HEAVY:-50}" \
	-v rtt_gross="${RTT_GROSS:-100}" -v turn_gross="${TURN_GROSS:-1000}" \
	-v rss_slack="${RSS_SLACK_KIB:-4096}" -v profile="$profile" '
function kv(a, from,  i, f) {
	for (i = from; i <= NF; ++i) {
		split($i, f, "=")
		a[f[1]] = f[2]
	}
}
function check(name, ok, detail) {
	printf "CHECK %s=%s %s\n", name, ok ? "pass" : "fail", detail
	failed += !ok
}
function ratio(a, b) {
	return b > 0 ? a / b : 0
}
$1 == "IDLE" && $2 == "SUMMARY" && $3 == "kind=all" { kv(idle, 4) }
$1 == "AFTER" && $2 == "SUMMARY" && $3 == "kind=all" { kv(aft, 4) }
$1 == "SUMMARY" && $2 == "kind=all" { kv(v, 3) }
$1 == "SUMMARY" && $2 == "kind=fresh-ping" { kv(fresh, 3) }
$1 == "CONTROL" { kv(c, 2) }
$1 == "LOOP" && $2 == "idle" && $3 == "turn" { kv(it, 4) }
$1 == "LOOP" && $2 == "load" && $3 == "turn" { kv(t, 4) }
$1 == "LOOP" && $2 == "after" && $3 == "turn" { kv(ta, 4) }
$1 == "LOOP" && $3 == "work" {
	work_seen = 1
	for (i = 4; i <= NF; ++i) {
		split($i, f, "[=/]")
		if (f[3] != "" && f[2] + 0 > f[3] + 0)
			over = over " " $2 ":" $i
	}
}
$1 == "FAIR" && $2 == "probe" { kv(fp, 3) }
$1 == "FAIR" && $2 == "jobs" { kv(fj, 3); jobs_fair = 1 }
$1 == "BG" {
	name = substr($2, 6)
	bgs[name] = 1
	for (i = 3; i <= NF; ++i) {
		split($i, f, "=")
		bg[name, f[1]] = f[2]
	}
}
$1 == "SOLO" {
	name = substr($2, 6)
	solos[name] = 1
	for (i = 3; i <= NF; ++i) {
		split($i, f, "=")
		solo[name, f[1]] = f[2]
	}
}
$1 == "MOCK" && $2 == "end" {
	line = $3
	gsub(/[{}"]/, "", line)
	n = split(line, f, ",")
	for (i = 1; i <= n; ++i) {
		split(f[i], g, ":")
		me[g[1]] = g[2]
	}
}
$1 == "mock:" && $2 ~ /^requests=/ { kv(mf, 2) }
$1 == "RESOURCES" {
	for (i = 4; i <= 6; ++i) {
		split($i, f, "=")
		res[f[1]] = f[2]
	}
	for (i = 8; i <= 10; ++i) {
		split($i, f, "=")
		fd[f[1]] = f[2]
	}
}
END {
	if (stall) {
		# Arrivals in the first 3/4 of the freeze wait over 1/4 of it.
		want = int(r * s)
		ok = v["slow"] >= 0.6 * want && c["slow"] <= 0.4 * want
		printf "VERDICT stall-check=%s stall_ms=%d frozen_arrivals=%d " \
		       "schedule_timed_slow=%d dispatch_timed_slow=%d\n",
		       ok ? "pass" : "fail", s * 1000, want, v["slow"], c["slow"]
		exit !ok
	}

	printf "RATIO load/idle p50=%.2f p99=%.2f p99_9=%.2f max=%.2f " \
	       "(idle p50_us=%s p99_us=%s; load p50_us=%s p99_us=%s max_us=%s) " \
	       "turn p99_9=%.2f max=%.2f (idle p99_us=%s; load p99_9_us=%s " \
	       "max_us=%s over_1ms=%s)\n",
	       ratio(v["p50_us"], idle["p50_us"]), ratio(v["p99_us"], idle["p99_us"]),
	       ratio(v["p99_9_us"], idle["p99_9_us"]), ratio(v["max_us"], idle["max_us"]),
	       idle["p50_us"], idle["p99_us"], v["p50_us"], v["p99_us"], v["max_us"],
	       ratio(t["p99_9_us"], it["p99_us"]), ratio(t["max_us"], it["p99_us"]),
	       it["p99_us"], t["p99_9_us"], t["max_us"], t["over_1ms"]
	printf "RATIO after/idle p50=%.2f p99=%.2f p99_9=%.2f max=%.2f " \
	       "(after p50_us=%s p99_us=%s max_us=%s) turn max=%.2f (after " \
	       "p99_9_us=%s max_us=%s over_1ms=%s)\n",
	       ratio(aft["p50_us"], idle["p50_us"]), ratio(aft["p99_us"], idle["p99_us"]),
	       ratio(aft["p99_9_us"], idle["p99_9_us"]),
	       ratio(aft["max_us"], idle["max_us"]), aft["p50_us"], aft["p99_us"],
	       aft["max_us"], ratio(ta["max_us"], it["p99_us"]), ta["p99_9_us"],
	       ta["max_us"], ta["over_1ms"]

	bad = v["err"] + v["timeout"] + v["lost"] + idle["err"] + idle["timeout"] + \
	      idle["lost"] + aft["err"] + aft["timeout"] + aft["lost"]
	check("errors", v["sched"] > 0 && aft["sched"] > 0 && !bad,
	      sprintf("probe sched=%d+%d failed=%d", v["sched"], aft["sched"], bad))
	lock = fresh["err"] + fresh["timeout"] + fresh["lost"]
	detail = sprintf("fresh-ping ok=%d failed=%d", fresh["ok"], lock)
	if ("churn" in bgs) {
		lock += bg["churn", "err"] + bg["churn", "timeout"] + bg["churn", "lost"]
		detail = detail sprintf(" churn ok=%d of %d", bg["churn", "ok"],
		                        bg["churn", "sched"])
	}
	check("lockout", !lock, detail)

	unanswered = 0
	detail = ""
	if ("idle" in bgs) {
		unanswered += bg["idle", "opened"] - bg["idle", "served"]
		detail = detail sprintf(" idle=%d/%d", bg["idle", "served"],
		                        bg["idle", "opened"])
	}
	split("malformed hostile dense", burst, " ")
	for (i = 1; i <= 3; ++i) {
		m = burst[i]
		if (!(m in bgs))
			continue
		# A body the stop cut short could not be answered.
		unanswered += bg[m, "sent"] - bg[m, "answered"]
		if (m == "dense")
			unanswered += bg[m, "sent"] - bg[m, "ok"]
		detail = detail sprintf(" %s=%d/%d", m, bg[m, "answered"], bg[m, "sent"])
	}
	if ("jobs" in bgs) {
		unanswered += bg["jobs", "failed"] + bg["jobs", "dropped"]
		detail = detail sprintf(" jobs completed=%d failed=%d dropped=%d",
		                        bg["jobs", "completed"], bg["jobs", "failed"],
		                        bg["jobs", "dropped"])
	}
	if ("hangup" in bgs) {
		unanswered += bg["hangup", "failed"] + bg["hangup", "dropped"]
		detail = detail sprintf(" hangup failed=%d dropped=%d",
		                        bg["hangup", "failed"], bg["hangup", "dropped"])
	}
	sum = mf["served"] + mf["cancelled"] + mf["active"] + mf["queued"]
	mock_ok = mf["requests"] == sum && !mf["active"] && !mf["queued"]
	check("answered", !unanswered && mock_ok, detail sprintf(" mock requests=%d " \
	      "served=%d cancelled=%d active=%d queued=%d", mf["requests"], mf["served"],
	      mf["cancelled"], mf["active"], mf["queued"]))

	open = 0
	detail = ""
	split("loris slowbody", hold, " ")
	for (i = 1; i <= 2; ++i) {
		m = hold[i]
		if (!(m in bgs) || dur < hold_s)
			continue
		open += bg[m, "opened"] - bg[m, "closed"]
		detail = detail sprintf(" %s closed=%d/%d", m, bg[m, "closed"],
		                        bg[m, "opened"])
	}
	if (("requests" in me)) {
		# No upstream call outlives its client; with only clients that hang
		# up, none is answered.
		open += me["active"] + me["queued"] > 0
		if ("hangup" in bgs && !("jobs" in bgs))
			open += me["served"] || !me["cancelled"]
		detail = detail sprintf(" upstream after the load: requests=%d " \
		                        "served=%d cancelled=%d active=%d queued=%d",
		                        me["requests"], me["served"], me["cancelled"],
		                        me["active"], me["queued"])
		if ("hangup" in bgs)
			detail = detail sprintf(" (hung up=%d)", bg["hangup", "hungup"])
	}
	check("limits", !open, detail)

	leak = fd["after"] > fd["idle"] + 2 || res["after"] > res["idle"] + rss_slack
	check("leaks", !leak, sprintf("rss_kib idle=%d after=%d fds idle=%d after=%d",
	      res["idle"], res["after"], fd["idle"], fd["after"]))

	unfair = fp["conn_p90_us_ratio"] > fair_conn || fp["kind_p90_us_ratio"] > fair_kind
	detail = sprintf("conn p90 median=%sus max=%sus ratio=%s (p99 ratio %s); " \
	                 "kind p90 median=%sus max=%sus ratio=%s (p99 ratio %s)",
	                 fp["conn_p90_us_median"], fp["conn_p90_us_max"],
	                 fp["conn_p90_us_ratio"], fp["conn_p99_us_ratio"],
	                 fp["kind_p90_us_median"], fp["kind_p90_us_max"],
	                 fp["kind_p90_us_ratio"], fp["kind_p99_us_ratio"])
	if (jobs_fair) {
		unfair += fj["calls_min"] < 1 && fj["calls_median"] >= 3
		detail = detail sprintf("; job calls min=%d median=%s max=%s",
		                        fj["calls_min"], fj["calls_median"], fj["calls_max"])
	}
	# A heavy mode with no body or batch done, while it had some alone, is
	# starved outright.
	split("pipeline malformed hostile dense", heavy, " ")
	for (i = 1; i <= 4; ++i) {
		m = heavy[i]
		if (!(m in solos) || !(m in bgs))
			continue
		x = ratio(bg[m, "p90_us"], solo[m, "p90_us"])
		unfair += x > fair_heavy || (solo[m, "n"] && bg[m, "p90_us"] == "")
		detail = detail sprintf("; %s p90 %sus, alone %sus: %.1fx", m,
		                        bg[m, "p90_us"], solo[m, "p90_us"], x)
	}
	check("fairness", !unfair, detail)

	if (stats == "full")
		check("work", work_seen && over == "", !work_seen ? "no statistics" :
		      over == "" ? "every unit within its bound" : over)
	else
		printf "CHECK work=n/a askai reports no units of work\n"

	turn_limit = turn_gross * it["p99_us"]
	if (rtt_gross * idle["p99_us"] > turn_limit)
		turn_limit = rtt_gross * idle["p99_us"]
	gross = ratio(v["p99_us"], idle["p99_us"]) > rtt_gross ||
	        ratio(aft["p99_us"], idle["p99_us"]) > rtt_gross ||
	        (stats != "none" && (t["max_us"] > turn_limit || !t["n"] ||
	                             ta["max_us"] > turn_limit || !ta["n"]))
	check("outliers", !gross, sprintf("p99 %.1fx idle, after %.1fx (limit %dx), " \
	      (stats == "none" ? "turns n/a" : "turn max %.1fx idle p99 turn, " \
	       "after %.1fx (limit %.0f us: %dx that or %dx the idle p99)"),
	      ratio(v["p99_us"], idle["p99_us"]), ratio(aft["p99_us"], idle["p99_us"]),
	      rtt_gross, ratio(t["max_us"], it["p99_us"]),
	      ratio(ta["max_us"], it["p99_us"]), turn_limit, turn_gross, rtt_gross))

	printf "VERDICT %s profile=%s failed_checks=%d\n", failed ? "fail" : "pass",
	       profile, failed
	exit failed > 0
}' "$tmp/gen.out"
verdict_rc=$?

if ((askai_rc || idle_rc || solo_rc || gen_rc || after_rc || mock_rc)); then
	printf '%s: FAIL: askai or the harness failed\n' "$self" >&2
	exit 1
fi
((!verdict_rc)) || exit 3
