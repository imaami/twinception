# Daemon core measurements

Load-harness results for the daemon core (`doc/core-design.md`): method,
verdict, and each build-plan stage's key numbers and findings, all from runs
on the machine below. Raw run logs stay out of the repository.

## Harness

`tests/load/` holds `gen.c` (the open-loop probe and the load modes),
`mock.c` (an upstream with slots, token rate, SSE and cancel counting) and
`run.sh`, whose header documents profiles, phases, output and environment.
`make load-build` builds it; `tests/load/run.sh PROFILE <askai> <dir>
[seconds]` runs a profile, `make load` the storm, `make check-load` (in
`make check`) `check`. `run.sh` exits 0 on a pass, 3 on a fail, and 1 when
askai or the mock did not exit 0 or the generator lost arrivals.

| Profile | Load beside the probe |
|---|---|
| idle | 5000 keep-alives, one answered ping each, then silence |
| jobs | 64 connections looping on tool calls; the mock has 1 slot, 50 ms |
| answers | 16 job connections with 512 KiB answers (S2b) |
| sse | 64 connections whose tool calls hang up 20 ms after the event stream opens (from S4 10–30 ms); the mock holds calls 60 s (S2b) |
| race | 16 job connections and 48 whose calls hang up 10–30 ms after the event stream opens, against a mock that answers each in 20 ms and closes the connection; churn at 200/s (S4) |
| loris | 128 loris heads and 128 slow bodies, one byte a second |
| slowread | 128 clients that pipeline 64 KiB of `tools/list` each and never read |
| pipeline | 4 MiB batches of 24 966 pings, back to back on one connection |
| hostile | bodies on new connections in a closed loop. Over the limits: 100 k-deep nesting, 400 k values, a 4 MB string. From S2a, within them: 1013 empty objects, 507 keys, a 1 MiB string, a 1 MiB tool call of escaped prose |
| dense | 1 MiB bodies, one at a time: 524 k zeros, 349 k empty objects, or 237 k members in objects of 512; askai has `--max-values 1000000` (S2b) |
| malformed | malformed requests on new connections in a closed loop; churn of fresh-connection pings at 1000/s |
| storm | 10 000 idle; 64 jobs; 64 each of loris, slow-body and slow-read clients; a 4 MiB pipeline; churn at 1000/s; malformed; hostile; mock 1 slot |
| check | every mode on a small scale (500 idle, 8 jobs, 4 hang-ups, 16 + 16 slow, 8 slow readers, a pipeline, churn at 200/s, hostile, dense, 256 KiB answers); askai has `--head-timeout 2 --body-grace 1 --max-values 1000000` (S2b) |

`baseline` is the probe alone; `stall` is the self-check under "Method".

## Verdict

From S2b the latency policy of design §8 and §11.5 judges every run: no
absolute latency fails one, and latencies are compared only as ratios to the
same run's idle baseline. A run's phases are the probe alone (`BASE` s, 3
by default); the heavy modes (pipeline, malformed, hostile, dense) alone
(`SOLO`); the load; and the probe alone again, for `AFTER_S` s while askai
keeps the load's memory, then (from S3m) under critical memory pressure,
which `run.sh` induces through `--test-pressure`, until askai has given it
back (`LEAK_S` s at most). `run.sh` prints `RATIO`, a `CHECK` per clause and
`VERDICT`.

| Clause | A run fails on |
|---|---|
| errors | a probe arrival that failed, timed out or was lost, in any phase; a background request sent whole but not answered (idle, malformed, hostile, dense; dense ones not answered 200); a tool call failed or cut off; mock counters that do not add up |
| lockout | a fresh connection not served: the fresh-ping probe kind, and churn |
| limits | loris or slow-body clients still open once their deadline has passed; an upstream call that outlived its client by 3 s; in the `sse` profile, where every client hangs up on its event stream, any call answered rather than cancelled |
| leaks | descriptors not back at the idle count + 2, or RSS not back within `RSS_SLACK_KIB` (4096), by the end of the after phase: what askai keeps for reuse it gives back under the induced pressure |
| fairness | a probe connection's p90 over `FAIR_CONN` (10) × the median connection's, each connection taking its turn of the arrivals; a probe kind's p90 over `FAIR_KIND` (10) × the median kind's; a tool-call connection with no call done while the median one did 3; a heavy mode's p90 batch or body over `FAIR_HEAVY` (50) × its p90 alone, or none done |
| work | any unit of work in a step over its bound (`loop.h`, design §1), in any phase, on either loop from S4; work that cannot be split counts in full |
| jobs | from S4, askai's count of jobs as it exits: submitted not completed + cancelled + rejected, or more upstream cancels than cancelled jobs |
| outliers | probe p99 in the load or the after phase over `RTT_GROSS` (100) × the idle p99; a turn in either, of either loop from S4, over `TURN_GROSS` (1000) × the idle phase's p99 turn of the client loop and over `RTT_GROSS` × its probe p99 |

Fairness judges the p90: a client held back is slow throughout, while a
machine hiccup hits everyone alike. `STATS=loop` serves S2a (no work line)
and `STATS=none` S0 and S1 (SIGUSR1 would end them); clauses they cannot
report are n/a. `MEMORY=trim` serves S0–S3, which gave memory back by
themselves once quiet and take no `--test-pressure`. Against ASan, which keeps freed memory, raise `RSS_SLACK_KIB`
and let LeakSanitizer judge leaks. S0–S2a were gated by design §8's former
absolute objectives; all verdicts below are the current one's.

## Method

- **Machine:** 32 logical CPUs, Linux 7.2.6-zz3, Debian, glibc. S0 and S1
  ran quiet (S1 counts only runs at a 1-minute load average below 1.0), S2a
  to S3m on a busy desktop (a browser, a GPU process, two `llama-server`).
- **Build and runs:** default `make` (clang 23.1.2, `-O3 -march=native
  -flto=auto -DNDEBUG=1`); harness from `make load-build` (clang, `-O2`).
  10 s of load a run; `check` 2 s idle + 5 s load, about 15 s in all.
- **Probe:** Poisson arrivals at 1 kHz over six kinds: the legacy `ping`,
  `tools/list` and `initialize`; the 2026-07-28 `server/discover` and
  `tools/list`; and `fresh-ping` on a new connection, the only kind that sees
  accept starvation. The others share 32 keep-alive connections, which since
  the review of S2b take the arrivals in turn.
- **Timing:** from each arrival's scheduled send, so coordinated omission
  cannot hide a stall; unanswered after 5 s is a timeout at its age; `sched =
  ok + err + timeout + lost`. A `CONTROL` histogram times the same arrivals
  from dispatch. At the default 50 µs timer slack, the generator's own late
  wake-ups made ping p50 77.1–77.6 µs, not 28.4–28.5 µs, so it uses 1 ns.
- **Sources:** from S2a, background modes connect from 127.N.x.y, 128 per
  address (askai caps an address at 256). Back-to-back storms filled the
  conntrack table and SYNs were dropped, so `run.sh` waits for room first.
- **Self-check:** `stall` stops askai for 320 ms (320 arrivals) and passes
  when at least 60 % of them take over 80 ms timed from the schedule and at
  most 40 % timed from dispatch. Schedule / dispatch: S0 251 / 73, S1
  243 / 72, S2a 244 / 73, S2b 252 / 74.
- **Older stages:** each stage's final binary re-ran through the S2b harness
  as it was before the S2b review (no solo or after phase), at load averages
  of 2.0–3.2. Profiles not re-run are judged from the stage's record, its
  baseline profile standing in for the idle phase: marked (rec.).

## S0 — baseline of the original askai

askai as it stood: a resumable HTTP/1.1 parser, `dstr` on the I/O path, one
`epoll` loop, the libcurl multi bridge, `MAX_CONNS = 64`. Load average 0.15;
gate "recorded", met. In the record, 5.0 s is the probe timeout, followed by
the number of fresh pings that timed out.

| Profile | Record: p99 / max | Verdict | Fails on (re-run) |
|---|---|---|---|
| baseline | 85.8 µs / 149.7 µs | pass | — |
| idle | 5.0 s (1674) | fail | errors (32 of 4129 idle served), lockout (0 of 1681 fresh pings), fairness (fresh-ping p90 77 506× the median kind's), outliers |
| jobs | 5.0 s (1673) | fail | errors, lockout, leaks (descriptors 10 → 30), fairness (a job connection with no call done, median 4.5), outliers |
| loris | 5.0 s (1664) | fail | errors, lockout, limits (0 of 128 + 128 closed), fairness, outliers |
| slowread | 5.0 s (1660) | fail (rec.) | lockout |
| pipeline | 744.5 ms / 763.0 ms | fail | outliers (p99 724 ms, 7 459× idle; connections equally slow, spread 1.06) |
| hostile | 31.0 ms / 35.7 ms | fail | outliers (325×), leaks (RSS 9.7 → 39.9 MB) |
| malformed | 14.7 ms / 66.4 ms | fail (rec.) | outliers (171×) |
| storm (worst of 3) | 5.0 s (1656) | fail | errors, lockout, limits, leaks, outliers |

- **Lockout:** 32 probe and 32 held connections fill `MAX_CONNS`; accepted
  clients get ping p99 61–75 µs, every new one times out. S1 drops the cap.
- **Pipeline stall:** a 4 MiB batch drains in 0.78 s, erased from one
  `dstr`'s front, while the others wait (probe p50 372 ms). S1 fixes it.
- **Hostile JSON** is parsed whole on the loop: probe p50 12 ms, RSS 40 MB.
- **Storm:** no tool call reaches the upstream, as the jobs never get an
  accept slot; 95 % of churn times out.

## S1 — `struct buf` in httpd

Gate "a 4 MiB pipeline delays others < 2 ms": met, at most 0.63 ms in six
quiet runs, while a batch drains in about 92 ms, 8× faster than in S0.
Unlike design §5, `buf_reserve` at its maximum moves the unread bytes however
few were consumed, so a parser never waits forever for the rest of a frame
(`buf.h` gives the cost).

| Profile | Record: p99 / max | Verdict | Fails on (re-run) |
|---|---|---|---|
| baseline | 85.2 µs / 133.7 µs | pass | — |
| idle (5000) | 84.7 µs / 113.6 µs | pass | — |
| jobs | 83.2 µs / 125.2 µs | fail | leaks (RSS 10.0 → 14.9 MB) |
| loris | 84.7 µs / 1.36 ms | fail | limits (0 of 128 + 128 closed) |
| slowread | 84.7 µs / 132.6 µs | pass (rec.) | — |
| pipeline (6 runs) | 103–113 µs / 0.17–0.63 ms | pass | — |
| hostile | 34.2 ms / 38.8 ms | fail | outliers (358×), leaks (RSS 9.9 → 35.1 MB) |
| malformed | 1.80 ms / 2.67 ms | pass (rec., 21×) | — |
| storm (3 runs) | 32.4–32.8 ms / 35.9–37.3 ms | fail | limits, leaks (RSS 9.9 → 41.2 MB), outliers (188×) |

- **No lockout:** the cap is gone; 10 000 idle are served in the storm, and
  the mock gets 585–586 tool calls (S0: 0).
- **No retention:** after 64 idle clients sent 976 KiB each, RSS is
  9 984 KiB, from 9 900 (S0: 72 720; without the `mallopt`: 10 896).
- **Review:** the ready pass had no per-turn limit (at 500 µs, the probe p99
  beside 64 pipelines fell from 2.53 to 1.53 ms); one tick closed 5000
  expired keep-alives in a 32.0 ms callback (now 64 a turn, max 1.35 ms).
- **Review, descriptors:** idle clients could starve upstream calls. Clients
  now get the soft `RLIMIT_NOFILE` less 64 (half below 128), less one per
  call in flight; past that, LRU eviction of idle keep-alives, and a spare
  descriptor for EMFILE. `tests/askai.py` checks it at a limit of 192.

## S2a — loop v2, limits and deadlines, failure isolation

Gate "loris closed; no turn > 1 ms; cheap-request max low; make check green;
sanitizers clean": partly met. Every slow client was closed by its deadline
and the storm's cheap-request max was 1.05–1.28 ms (S1: 37 ms), but one of
six final storms had a 2.12 ms turn, traced to preemption; askai's own steps
stayed under 0.65 ms. Load average 2.0–2.9.

| Profile | Record: p99 / max, turn max | Verdict | Fails on (re-run) |
|---|---|---|---|
| baseline | 100.1 µs / 164.1 µs, 211 µs | pass | — |
| idle (5000) | 100.6 µs / 172.7 µs, 557 µs | pass | — |
| jobs | 101.6 µs / 245.1 µs, 576 µs | fail | leaks (RSS 9.8 → 14.8 MB) |
| loris | 98.6 µs / 1.10 ms, 205 µs | pass | — |
| slowread | 98.6 µs / 155.6 µs, 524 µs | pass (rec.) | — |
| pipeline | 107.3 µs / 193.3 µs, 274 µs | pass | — |
| hostile (seven kinds) | 515.1 µs / 1.02 ms, 602 µs | pass | — |
| malformed | 129.3 µs / 252.5 µs, 170 µs | pass (rec.) | — |
| dense (re-run only) | — | fail | outliers (p99 270× idle; a 29.3 ms turn, 798× the idle p99 turn), leaks (RSS 10.1 → 282.9 MB) |
| storm (6 runs) | 435.2–494.6 µs / 1.05–1.28 ms, 760–2120 µs | fail | leaks (RSS 9.8 → 20.9 MB) |

- **Deadlines:** loris heads get 408 10 s after their first byte, slow bodies
  at 5 s; bodies over the limits are refused early (25 590 in 10 s, 10 MB).
- **Malformed tail was the harness:** from one source address the flood
  likely reuses TIME_WAIT ports; S1's askai passes from many (p99 122 µs).
- **Long turns:** 20–60 curl starts took 0.54–0.97 ms in one step (now two a
  step); a loopback reset 1.67 ms in `close()` (now a lingering close).
  Preemption remains: each turn over 0.9 ms had 1–9 context switches.
- **Failure isolation:** with `epoll_ctl` failing for upstream sockets
  (`tests/epfail.c`), S1 exits; S2a answers each call with an error.
- **Review (13, all fixed):** one address could hold the whole 256 MiB body
  budget (now the token is checked first, and `--client-budget` gives an
  address a quarter); a value limit of body / 64 let 16 383 empty objects
  take a 6.70 ms step (now 1024 values, ≤ 0.37 ms, and a 1 MiB tool call's
  step fell from 4.79 to 0.46–0.59 ms); steps queued by steps starved behind
  callback work, so new connections waited 5.0 s (now the two take turns:
  5.6–7.2 ms); refusing 32 clients took a 2.12–2.21 ms step (now 161–183 µs);
  the client table rehashed in 1.54 ms (now 65 536 fixed chains); slices
  split UTF-8 characters; long string ids (now ≤ 1 KiB); no `TCP_NODELAY`.
- **Deviations from the design:** bulk steps start until 650 µs, not 1 ms
  (at 750 µs, five of six storms had a 1.00–1.98 ms turn); one step per
  entry a turn, as cycling let a pipeline hold every turn; a lingering close
  (half-close, discard input up to 2 s); the principal is the client address
  (IPv4, IPv6 /64); the per-tool body limit is `--max-body`; requests are
  served under 256 KiB of pending output; while a send blocks only the write
  deadline counts.

## S2b — stream interface, chunked bodies, SSE, sliced JSON, work units

Gate "dense JSON in bounded slices, others degraded only in proportion; SSE
calls cancel on close; the §8 verdict on every profile; make check green;
sanitizers clean": met after the review, but for one long step, glibc's
release of a dense load's memory (see "Open decisions"). Load average
2.1–3.5; numbers from the reworked harness after the review. Design §1, §3
and §5 record the deviations ("as built in S2b") but one: a body found over
a JSON limit once all in keeps its connection (S2a closed it).

Every run passed. Times in µs unless marked; spread: the largest probe
connection's p90 over the median's; after: the after phase's longest turn.

| Profile | Idle p99 | p99 | p99 / idle | Spread | Turn max | After |
|---|---|---|---|---|---|---|
| baseline | 99.1 | 99.1 | 1.0 | 1.08 | 269 | 225 |
| idle (5000) | 100.6 | 101.1 | 1.0 | 1.06 | 548 | 517 |
| jobs | 99.6 | 100.1 | 1.0 | 1.06 | 725 | 483 |
| answers | 97.5 | 494.6 | 5.1 | 1.16 | 650 | 209 |
| loris | 97.5 | 99.6 | 1.0 | 1.08 | 305 | 169 |
| slowread | 98.0 | 101.6 | 1.0 | 1.07 | 527 | 382 |
| pipeline | 98.0 | 132.6 | 1.4 | 1.08 | 235 | 209 |
| hostile (3 runs) | 97.0–102.1 | 530–555 | 5.2–5.6 | 1.48–1.64 | 724–741 | 151–233 |
| malformed + churn | 98.6 | 128.8 | 1.3 | 1.04 | 275 | 270 |
| dense (5 runs) | 97.5–102.1 | 224.8–235.0 | 2.2–2.4 | 1.05–1.12 | 294.2–824.2 | 15.9–18.9 ms |
| sse (3 runs) | — | — | 9.2–9.4 | — | 754–782 | — |
| storm (3 runs) | 98.0–99.6 | 575.5–587.8 | 5.9–6.0 | 1.09–1.20 | 797.2–1054.5 | 986–1 039 |
| check (5 runs) | — | — | 4.9–5.2 | 1.12–1.21 | 669–720, one 3.7 ms step | 19.4–22.3 ms |

- **dense:** slices ≤ 1 152 bytes and 256 values, each body 1.1× its time
  alone, RSS 297 MB back to 11 MB; the S2a binary: p99 385× idle, 283 MB
  kept. **sse:** every upstream request cancelled, none served. **storm:**
  heavy modes 2.8 (pipeline), 3.0–3.4 (hostile), 5.9–10.4 (malformed) times
  their time alone.
- **check beside 32 CPU hogs** (load 6–39): six of six passed, at 13.8–17.9×
  the idle p50 and spreads of 1.05–1.09 (before the review: 13–66×,
  1.05–1.19); the trim took 53–67 ms, passing only by the 100× probe arm.
- **Campaign:** 16 KiB of `{},` is 5 461 objects, about 1 ms, so slices
  also end at 256 values; 116 k keys made rehash steps of up to 11.1 ms, so
  `--max-members` is 1024 (its last rehash under 0.1 ms); one entry per loop
  freeing trees let them pile up to 2.3 GB, so each request frees its own
  before its stream ends; a half-close cancelled cheap requests left for a
  bulk step (7 of 8000 answered), so RDHUP cancels only after `waiting()`.
- **Review (12, all held):** answers over `--max-output` were dropped (now a
  response may pass it while it is all the pending output); the work clause
  could not fail (unsplittable work such as json-c's string copies now
  counts in full as `copy`, bound 1 MiB); the 50-sample cut hid a starved
  connection (300 ms late passed at 1.08; with arrivals in turn it shows
  61 032), and heavy modes are now judged against their solo phase (1.0–10.4×
  measured); a 1 MB number took 1.98 ms (now ≤ 1 KiB); the trim was a 16.5 ms
  step no phase saw (now the after phase); a 16 MB answer parsed in one
  35.8 ms step (now sliced); a split Accept got 400; configuration json-c
  values were shared with non-atomic counts (now copied).

## S3 — brainstem, loop thread, logger, blockers, metrics, watchdog

Gate "stalled stderr harmless; an injected stall reported; signals end a
stuck loop within the grace period; `/metrics` access-controlled; blocker
completions; the §8 verdict on every profile; make check green; sanitizers
clean, TSan included": met. Design §1, §2 and §6 give what was built ("as
built in S3") and §11.1 how the main thread's role was settled.

The table is from after the review of S3 (below), at load average 0.9–1.8;
the campaign before it, at 2.2–3.5, gave the same picture. Every run passed;
the stall self-check gave 231 / 71 (schedule / dispatch). Columns as for
S2b.

| Profile | Idle p99 | p99 | p99 / idle | Spread | Turn max | After |
|---|---|---|---|---|---|---|
| baseline | 101.6 | 99.1 | 1.0 | 1.07 | 300 | 203 |
| idle (5000) | 96.5 | 99.6 | 1.0 | 1.06 | 514 | 515 |
| jobs | 99.1 | 102.7 | 1.0 | 1.08 | 671 | 335 |
| answers | 97.5 | 502.8 | 5.2 | 1.13 | 689 | 290 |
| sse | 101.6 | 948.2 | 9.3 | 1.47 | 791 | 397 |
| loris | 99.6 | 101.1 | 1.0 | 1.09 | 252 | 251 |
| slowread | 98.0 | 99.6 | 1.0 | 1.07 | 577 | 257 |
| pipeline | 98.6 | 130.3 | 1.3 | 1.09 | 327 | 187 |
| hostile | 99.6 | 546.8 | 5.5 | 1.33 | 738 | 240 |
| dense | 98.6 | 229.9 | 2.3 | 1.08 | 690 | 17.3 ms |
| malformed + churn | 100.1 | 123.1 | 1.2 | 1.04 | 220 | 182 |
| storm (3 runs) | 97.5–101.6 | 546.8–583.7 | 5.6–5.8 | 1.10–1.13 | 807–924 | 926–1 014 |
| check | 98.0 | 461.8 | 4.7 | 1.13 | 822 | 20.6 ms |

- **Descriptor table:** the first campaign had steps of 15.5 ms (idle),
  6.5 ms (loris), 12.5 ms (storm) and 9.8 ms (check), with almost no work in
  them. Once threads share the descriptor table, the kernel waits for an RCU
  grace period each time it grows: 7–11 ms per doubling from 64 to 4096 in a
  test program with four threads, 0.01 ms without them or with the table
  grown first. The brainstem now grows it to 65 536 before any thread
  starts; the table above is from after that.
- **Stalled stderr:** askai's stderr a full 4 KiB pipe nobody reads, then
  2434 SIGUSR1 in 5 s, each five lines of statistics: 2286 lines went into
  the rings, 9885 were dropped and counted; probe p99 102.1 µs, max 139.5 µs,
  against 104.7 / 139.7 µs with stderr a file, and 97.0 µs idle in both;
  SIGTERM exits 0 after the 1 s the logger is given.
- **Watchdog:** a 300 ms injected stall (`--test-stall`) is reported once, at
  22.7 ms. 30 SIGSTOPs of 300 ms under a pipeline load: no report, 30 late
  wakes, while the loop's own turn histogram counts the 30 frozen turns.
- **Forced exits:** SIGINT 0.2 s into a 60 s stall exits 3 after 0.81 s, as
  the turn reaches 1 s; a second SIGTERM exits 3 within 50 ms.
- **Drain:** 100 idle connections and 20 tool calls waiting upstream, half on
  event streams: SIGTERM exits 0 in 4 ms, all 20 calls ending with the
  shutdown error and the mock seeing their requests cancelled.
- **Lookups:** strace shows askai's four `clone3` calls (logger, two
  blockers, loop) and none while it looks up `localhost` or the host's name
  on a blocker; curl's own resolver started two threads for the same lookup.
- **Metrics:** 26 224 bytes, rendered in five bulk steps of at most 78 µs.
- **Sanitizers:** `tests/askai.py` (20 suites) passes under ASan/UBSan,
  clang's TSan and gcc's TSan, and the check profile under ASan/UBSan and
  clang's TSan, with no report. Under gcc's TSan, about ten times slower,
  the check profile fails "answered" now and then, with no report (3 runs
  of 3 before the review, 2 of 4 after): a hostile body's reply begins
  after the generator's fixed 2 s wait (once at 2.13 s), and a scratch copy
  waiting 20 s passed all 3 runs. TSan found three things, all fixed:
  glibc's `mtx_t`/`cnd_t` hide the blocker queue's lock from it (now POSIX's
  own); the logger, finished but unjoined at a forced exit, was a thread
  leak (now detached); and freeing the logger on that path raced threads
  still logging (no longer freed there).
- **Review (19 findings, all confirmed; one part declined):** the drain did
  all its work in the order's handler, one turn of 50–100 ms for 10 000 idle
  keep-alives, 10–25 ms for 2000 waiting tool calls and 250–500 ms for
  40 000; it now takes bulk steps of 32 closes or 8 calls, with no turn over
  0.5 ms (1 ms at 40 000, where 31 of 1257 steps passed 250 µs). With
  100 000 descriptors open, counting them made the watchdog late every
  second (10 late wakes in 10 s), so a stuck loop met the grace period
  (exit after 5.06 s); now 0 late wakes and 0.86 s. A shutdown signal
  ignored on entry (`nohup`) ended askai; a teardown of 1.5 s after the last
  turn counted as a stuck turn (exit 3); a third signal made the forced exit
  spin (0.99 s of CPU in 1.05 s); from loopback, a `Host` naming another
  name read `/metrics`, as a page whose name was rebound to 127.0.0.1 would;
  the brainstem's `opendir` called `malloc` about once a second in the arena
  whose lock `malloc_trim` takes. Under TSan, exits raced on the eventfd
  (2 of 300 start-and-SIGTERM runs) and leaked finished threads on a second
  signal (8 of 8) and with a blocker stuck in `getaddrinfo` (exit 66). All
  fixed: since, clang's TSan saw none in 300 start-and-SIGTERM runs, 60 runs
  each of a second signal, a stall and statistics before SIGTERM, and 3
  stuck blockers, nor gcc's TSan and ASan in 20 of each. Declined: caching
  the upstream URL's parse, at 0.23 µs a tool call, which libcurl repeats
  for every transfer anyway.

## S3m — memory retention

Gate, under the latency policy: "no allocator call for buffers per request
once warm, in steady keep-alive requests and in tool calls; RSS plateaus at
the working set's peak without pressure and is not trimmed; under induced
pressure what is kept goes back, RSS falling toward the idle phase's, in
bounded steps and fairly; the §8 verdict on every profile, with what askai
keeps not taken for a leak; make check green; sanitizers clean": met. Design
§5 gives the policy and what was built, §11.6 the user's directive.

**Allocator calls** per request, counted by an `LD_PRELOAD` shim that puts
each call down to the object it returns to (askai; json-c; libcurl; libc,
which calls them on behalf of the libraries), after 200 requests of a kind
and over 2000 more on one keep-alive connection, the mock answering at once.
Allocations (`malloc`, `calloc`, `realloc`) / frees:

| Request | askai, S3 | askai, S3m | json-c | libc | libcurl |
|---|---|---|---|---|---|
| ping, tools/list | 10 / 6 | 0 / 0 | 8 / 15 | 8 / 1 | — |
| initialize | 11 / 6 | 0 / 0 | 20 / 39 | 20 / 1 | — |
| server/discover | 10 / 6 | 0 / 0 | 18 / 33 | 16 / 1 | — |
| tools/call, JSON or event stream | 18–21 / 14–15 | 4 / 6 | 49–56 / 83–96 | 42–48 / 2 | 47 / 45 |
| GET /metrics | 12 / 4 | 0 / 0 | — | — | — |

Of a tool call's 4 / 6 at askai's call sites none is a buffer: http.c's
request (1 / 1) and two watches of the upstream socket (2 / 2) are objects,
and libcurl's URL parse returns there through tail calls (1 / 3). json-c's,
and libc's for it, are its values and keys: the known remaining churn.
libcurl's are its transfer's: reusing easy handles saved 3 of its 47 in a
trial, not adopted. With a 300 KB prompt and a 65 536-token answer, a tool
call made 22 memory syscalls in S3 (9 `mmap`, 9 `munmap`, 2 `mremap`, 2
`madvise`; `strace -c`) and none in S3m, and askai's own calls fell from 23 /
17 to 4 / 6, json-c's from 68 / 115 to 49 / 109: since the review every
tokener is pooled, where each such call built two and regrew their string
buffers (12 `realloc`s, 1.5 MB). Cheap requests made no memory syscall in
either.

**glibc's tuning** (design §5), in a test program whose thread allocates
230 MB in 48-byte blocks: its arena grew with 58 757 `mprotect` calls by
default, 0.18 s, and with 7 at `M_TOP_PAD` 64 MiB, 0.12 s, as that pad makes
a heap writable whole when glibc creates it; a pad of 1 or 4 MiB saved none,
as glibc pads only a heap's creation. Without the pad, glibc also deletes a
thread's heap once all of it is free: one `check` run in five kept 336 of
its 361 MB peak. Since the review the pad applies only without RLIMIT_DATA,
RLIMIT_AS or strict overcommit. Freed first in first out under
`M_TRIM_THRESHOLD` −1, the blocks left a 64 MiB heap resident that
`malloc_trim(0)` left too (98.6 MB before and after), and that a free of
64 KiB under a low threshold and pad gave back (33 MB, the program's own
array).

**Retention and release:** every profile kept its peak through the after
phase's unpressured part (`kept` = peak), and with RSS over idle + 4 MiB the
induced critical pressure gave it back in one relief. After `dense`
(299 MB), the relief released the stash's 1.1 MiB, 256 units a step at
most, then trimmed in one 16.7–18.8 ms step, down to 12.0–12.5 MB; later
reliefs, every 10 s while the pressure lasted, trimmed in 0.1 ms. At high
pressure a relief releases what went unused for a window (10–20 s): one a
second after `dense` released nothing, its blocks just used, but its trim
gave back the 285 MB json-c had freed (298 → 13.2 MB, 17.2 ms); the one
20 s later released the stash's unused 1.0 MiB.

**Where the trim runs**, three `check` runs each: in a loop step the after
phase's probe max was 18.0–21.2 ms and its turn max 20.9–21.9 ms; on a
blocker 20.3–21.6 ms and 20.3–21.6 ms, the loop waiting for its arena's lock
in the turn. It stays a loop step.

The table is from one run of each profile with the binary after the review,
at load average 1.2–2.0; the campaigns before the review gave the same
picture. Every run passed; the stall self-check gave 258 / 74. Columns as for
S2b, then RSS in MB: idle, peak, kept through the unpressured after phase,
and after the induced pressure (not needed where RSS stayed within 4 MiB of
idle).

| Profile | Idle p99 | p99 | p99 / idle | Spread | Turn max | After | RSS idle / peak / kept / after |
|---|---|---|---|---|---|---|---|
| baseline | 101.6 | 100.1 | 1.0 | 1.05 | 154 | 195 | 11.9 / 11.9 / 11.9 / 11.9 |
| idle (5000) | 99.6 | 100.1 | 1.0 | 1.08 | 531 | 515 | 11.7 / 13.7 / 13.7 / 13.7 |
| jobs | 99.1 | 100.6 | 1.0 | 1.11 | 678 | 517 | 11.8 / 17.0 / 17.0 / 12.3 |
| answers | 100.1 | 224.8 | 2.2 | 1.12 | 463 | 1 091 | 11.6 / 26.1 / 26.1 / 12.0 |
| sse | 102.7 | 907.3 | 8.8 | 1.29 | 729 | 436 | 11.5 / 16.9 / 16.9 / 12.2 |
| loris | 100.1 | 100.6 | 1.0 | 1.07 | 280 | 200 | 11.7 / 13.7 / 13.7 / 13.7 |
| slowread | 94.5 | 100.1 | 1.1 | 1.07 | 530 | 245 | 11.8 / 13.4 / 13.4 / 13.4 |
| pipeline | 98.0 | 124.7 | 1.3 | 1.06 | 281 | 157 | 11.6 / 11.6 / 11.6 / 11.6 |
| hostile | 98.6 | 188.9 | 1.9 | 1.09 | 575 | 339 | 11.7 / 16.2 / 16.2 / 11.9 |
| dense | 95.0 | 227.8 | 2.4 | 1.08 | 298 | 18.1 ms | 11.7 / 298.6 / 298.6 / 12.1 |
| malformed + churn | 95.5 | 127.7 | 1.3 | 1.08 | 255 | 215 | 11.8 / 11.8 / 11.8 / 11.8 |
| storm | 99.1 | 293.9 | 3.0 | 1.09 | 848 | 1 510 | 11.8 / 31.4 / 31.4 / 12.8 |
| check | 97.5 | 439.3 | 4.5 | 1.16 | 798 | 21.4 ms | 11.9 / 357.9 / 357.9 / 12.9 |

- **Large buffers:** with no map and unmap per buffer over 128 KiB, the p99
  beside 256 KiB answers fell from 5.2× idle (S3) to 2.2–2.4×, beside
  hostile 1 MiB bodies from 5.5× to 1.9×, and in the storm from 5.6–5.8× to
  2.8–3.1× (three campaigns).
- **Sanitizers:** `tests/askai.py` (21 suites, the memory suite new) passes
  under ASan/UBSan and clang's TSan, and the check profile under both
  (`RSS_SLACK_KIB` raised, as their allocators keep what is freed), with no
  report and askai exiting 0, before the review and after.
- **Deviations:** a tool call still makes 3 allocator calls of askai's own
  (http.c's objects above); the trim stays a loop step, which the brainstem
  orders.
- **Review (19 findings, two of them the same; all confirmed, three parts
  declined):** before → after, each measured on the same machine.
  - Spare requests were reused first in, first out, each keeping its
    answer's text: 250 pings at once, then 100 calls with 1 MB answers, left
    RSS at 113 MiB; reused last in, first out, 19 MiB.
  - Each stash class kept its own peak: waves of 32 MiB of bodies in seven
    classes kept 216 MiB (RSS 229 MiB); within twice the peak held, 79 MiB
    descending and 86 MiB ascending (RSS 94 and 102 MiB).
  - Parses over 16 KiB freed their tokener (above).
  - Under critical pressure a trimmed buffer's large block went into the
    emptied stash and stayed; storage given up then goes back at once.
  - The SIGUSR1 statistics' fresh `calloc` was memset under the new mmap
    threshold, 232 kB resident more; now zeroed in place, no growth.
  - The cgroup input counted reclaimable file cache: a 192 MiB scope full of
    it read critical, with a relief every 10 s; now it reads 5–6 MB, and
    no relief runs.
  - `M_TOP_PAD` 64 MiB made thread heaps writable whole: under RLIMIT_DATA
    128 MiB, all of 40 pings with 1 MB bodies failed (3 without a reply, 21
    with a 500, 16 with a 400), under RLIMIT_AS 160 MiB 37. Under such
    limits, or strict overcommit, the pad is now glibc's: all 40 passed
    under each, VmData 79–82 MB (164 MB with the pad, unlimited).
  - The default `--memory-high` followed the fullest cgroup's limit: in a
    5 GiB cgroup within an 8 GiB scope it moved from 512 to 320 MiB as RSS
    grew; it is now the lowest limit's sixteenth, 320 MiB throughout.
  - A release step counted blocks: sixteen 32 MiB blocks went in one 19 ms
    step; weighed per 128 KiB, sixteen steps of 1.1–1.4 ms.
  - At high pressure an episode's first relief gave back nothing of a stash
    idle for 20 s; with windows of reuse it gave back all 42.3 MB.
  - Spare requests and pooled tokeners' storage now show at `/metrics`, a
    missing MemAvailable no longer reads as RAM all used, and the memory
    suite holds a waiting tool call, a pipelined head and a slow reader under
    critical pressure, whose buffers move (2 048, 2 715 and 194 636 bytes).
  - Declined: freeing a large answer's text as its request ends, an
    allocation and a free per call, which reuse last in, first out makes
    needless; a pressure input for RLIMIT_DATA and RLIMIT_AS, as under them
    askai now fails no request S3 served, and a relief lowers neither (glibc
    keeps a thread's heap writable); glibc's free memory as a metric
    (`mallinfo2` walks every arena under its lock).

## S4 — handles, jobs and home

Gate "TSan clean under churn; job accounting holds; the §8 verdict on the
check and storm profiles, ratios to idle beside S3m's; make check green;
warning-free gcc and clang builds": met. Design §1, §2, §4 and §6 give what
was built ("as built in S4"), §11.1 the decision on home.

The table is from one run of each profile with the final binary (after the
review of S4, below), at load average 0.8–1.5, and 1.5–11.7 for storm and
check, as other programs on the machine got busy; every run passed, and the
stall self-check gave 241 / 72. Columns as for S3m, with home's longest turn
in the load and the after phase.

| Profile | Idle p99 | p99 | p99 / idle | Spread | Turn max | Home load / after | After | RSS idle / peak / kept / after |
|---|---|---|---|---|---|---|---|---|
| baseline | 92.4 | 90.4 | 1.0 | 1.07 | 234 | 24 / 30 | 194 | 11.9 / 11.9 / 11.9 / 11.9 |
| idle (5000) | 93.4 | 93.4 | 1.0 | 1.08 | 516 | 21 / 29 | 538 | 11.9 / 13.9 / 13.9 / 13.9 |
| jobs | 92.4 | 90.4 | 1.0 | 1.07 | 522 | 247 / 447 | 167 | 11.8 / 17.1 / 17.1 / 12.5 |
| answers | 90.9 | 185.9 | 2.0 | 1.17 | 541 | 354 / 895 | 635 | 11.9 / 26.0 / 26.0 / 12.4 |
| sse | 89.9 | 205.3 | 2.3 | 1.26 | 533 | 326 / 536 | 266 | 12.0 / 17.2 / 17.3 / 12.8 |
| race | 96.0 | 94.5 | 1.0 | 1.06 | 457 | 251 / 616 | 208 | 12.0 / 17.1 / 17.1 / 12.7 |
| loris | 91.4 | 91.9 | 1.0 | 1.10 | 245 | 21 / 29 | 156 | 12.0 / 13.9 / 13.9 / 13.9 |
| slowread | 91.4 | 96.0 | 1.1 | 1.10 | 524 | 22 / 34 | 174 | 12.0 / 13.6 / 13.6 / 13.6 |
| pipeline | 96.5 | 119.6 | 1.2 | 1.10 | 268 | 23 / 27 | 231 | 11.9 / 12.0 / 12.0 / 12.0 |
| hostile | 94.5 | 179.7 | 1.9 | 1.08 | 450 | 24 / 273 | 188 | 12.0 / 16.4 / 16.4 / 12.2 |
| dense | 91.4 | 228.9 | 2.5 | 1.14 | 280 | 25 / 18.7 ms | 18.4 ms | 12.0 / 292.2 / 292.2 / 12.5 |
| malformed + churn | 92.4 | 125.7 | 1.4 | 1.06 | 201 | 21 / 22 | 202 | 11.8 / 11.9 / 11.9 / 11.9 |
| storm | 102.7 | 277.5 | 2.7 | 1.08 | 1.8 ms | 306 / 962 | 638 | 12.1 / 30.7 / 30.7 / 13.1 |
| check | 90.9 | 420.9 | 4.6 | 1.13 | 1.0 ms | 183 / 24.7 ms | 23.9 ms | 12.1 / 358.8 / 358.8 / 13.1 |

Beside S3m (the commit before S4), three rounds of each profile under S4's
harness, the binaries taking turns, at load average 1.3–6.6 (ranges over
the rounds; turn max is the client loop's):

| Profile | p99 / idle, S3m | S4 | Spread, S3m | S4 | Turn max, S3m | S4 |
|---|---|---|---|---|---|---|
| check | 4.21–4.63 | 4.23–4.56 | 1.08–1.18 | 1.12–1.15 | 0.96–1.99 ms | 0.83–0.98 ms |
| storm | 3.04–3.11 | 2.85–2.90 | 1.06–1.11 | 1.07–1.15 | 0.78–2.76 ms | 0.69–0.78 ms |
| race | 2.10–2.17 | 1.00–1.02 | 1.11–1.13 | 1.05–1.10 | 0.68–1.24 ms | 0.41–0.52 ms |
| sse | 4.72–5.00 | 2.07–2.10 | 1.10–1.18 | 1.19–1.32 | 0.67–0.78 ms | 0.52–0.60 ms |

`make load` passed at 2.9× idle, turn max 0.59 ms, home's 0.23 ms.

- **Off the client loop:** with libcurl on home, the client loop no longer
  starts, reads or cancels upstream transfers. Beside hang-ups the probe
  barely moves: `sse` 2.1× idle against S3m's 4.7–5.0× in the same rounds,
  `race` 1.0× against 2.1×. (S3m's own table gave `sse` 8.8×, with the
  harness of the time, whose hang-ups all came 20 ms in.)
- **Long turns:** both binaries show a turn of 1–2 ms now and then. In a
  10 s storm the client loop was preempted 56 063 times (S3m's loop
  58 814), home 100 times: they are the busy machine's, not home's.
- **Allocator calls** (S3m's shim, 2000 tool calls on one connection, in
  JSON and on an event stream): askai's own code makes 4 / 6 a call, as in
  S3m, none of them for the job, its handle or its buffers: the job is the
  spare one, and home's stash gives its result storage (2199 hits, 1 miss
  in 2200 calls). Cheap requests: 0 / 0.
- **Jobs:** every run's count held at exit, submitted = completed +
  cancelled + rejected with none in flight, and the mock saw no more cancels
  than askai cancelled jobs: `sse` 29 602 of 29 602 calls cancelled, all
  upstream (a job cancelled before home sends it never reaches the upstream:
  the review of S4 saw 6 of 15 498 so); `race` 33 481 submitted, 20 698
  completed, 12 783 cancelled, of which the mock saw 12 755, the rest
  cancelled after its answer had gone out. `tests/askai.py` checks,
  besides, that the calls a client loop cancelled while they waited match
  the completions it dropped as stale.
- **Races:** stopping askai while an answer arrives and its client hangs up
  makes home finish the job in 8 of 10 tries, its completion then finding
  the call's handle stale, and cancel it in 2; the `race` profile's
  hang-ups, 10–30 ms in, against a 20 ms upstream, do both by the thousand.
- **Home's turns:** the first version handled each SUBMIT and CANCEL by
  calling into libcurl at once, some tens of µs each, and with the inbox's
  256 messages a turn made home turns of 3.3–4.6 ms in `race`; its messages
  now only admit, refuse or move jobs, and bulk steps start and cancel them
  eight at a time: 0.23–0.30 ms. Home's after phase includes its own trim,
  which, under pressure only, also gives back what the client loop's arena
  has free.
- **Sanitizers** (clang, final code): `tests/askai.py`, 22 suites, under
  ASan/UBSan and under TSan with `halt_on_error`, reports written to files:
  none, every askai exit as expected; the check, race, jobs and sse
  profiles under both, malformed with churn under TSan and answers under
  ASan: every verdict passes, no report; and 30 stress runs that stop
  askai, with SIGTERM or two signals, 0.8–2.3 s into 32 tool calls, 32
  hang-ups and 300 connections a second against an upstream of 1–60 ms (20
  under TSan, 10 under ASan): every exit 0, every job accounted for, 8 jobs
  refused while home drained, no report.
- **Harness:** the hang-up mode waits 10–30 ms rather than 20; the `race`
  profile; the mock's `-c`, as the first `race` under TSan kept 5 idle
  upstream connections in libcurl's cache and failed the descriptor check
  (S6 caps the cache); `answers` under TSan fails that check the same way
  (17–18 descriptors, idle + 2 being 16), with or without the review's
  fixes, and is not among the sanitizer runs; home's statistics in the
  `work` and `outliers` clauses; the `jobs` clause. A hostile body over a
  limit is answered before it is all sent, and one that the generator's
  stop cut short was counted answered but not sent (171 of 170 in one
  `check`): it now counts neither way.
- **Review of S4**, each fix measured on the plain build:
  - Home counted no work as it took answers in, so the `work` clause passed
    it unseen; counted, a step took in up to 352 KiB. Home now takes 64 KiB
    a step and has libcurl hold the rest back: `recv` 65 536 / 65 536 in
    `answers` and `check`, as many calls done as before.
  - A job a drain cancelled kept its partial answer, which the front copied
    in its completion handler: with the client loop's drain held back
    0.3 s, 16 of 16 calls copied 3 MB each; now none does.
  - With 1 in 10 of home's allocations failing, libcurl's included, a
    transfer lost its timeout and its job stayed in flight past its deadline
    (`-T 3`: 3 of 3 runs); home now ends running jobs at their deadline
    itself: 4 of 4 runs accounted for every job, the stuck calls timing out
    after 3 s.
  - A loop whose release ended before another loop's walk of the arenas
    began now leaves its own walk out: 1 of 4 critical reliefs after 16
    cut-off 3 MB answers; in the others the second release ended later, and
    its walk was needed.
  - A client loop that could not allocate its server exited 1 without a
    reason; it says so now.
  - Tests: `tests/handle.c` (a slot named again under a stale handle,
    another loop's handle, the generation's wrap, growth), which fails when
    the generation does not advance, where `check_jobs` passed; home's
    relief under critical pressure; home's `recv` within its bound, and a
    17 MiB answer cut off at 16 MiB.
- **Deviations:** see design §4 (CANCEL also lets go, RETURN, four messages
  a job; home ends jobs at their deadline; answers paused at a step's
  share) and §5 (no storage moves between stashes; trims take turns).

## Open decisions

Measured, not settled, and left as they are:
- **Allocator and the trim:** glibc, tuned to keep what is freed (S3m),
  gives it back only under memory pressure, in one `malloc_trim(0)` step of
  about 50 µs per MB: 15.9–18.9 ms after `dense` (297 MB), 19.4–22.3 ms after
  `check` (340 MB), 53–67 ms beside 32 hogs (S2b); the verdict passes it. The
  tuning leans on glibc's internals (design §5). Trimming every 512 to 32 768
  values freed cost up to 1.6 ms a call; jemalloc's background purge kept
  5.1–5.2 MB over idle in `check` (a leak) and had 9.2 and 11.0 ms turns;
  mimalloc purged in a 19.8 ms step.
- **`--max-values` default (1024):** slices bound steps at any count (`dense`
  at 1000000 passes), but json-c takes about 0.65 KiB per empty object, so
  memory and the trim grow with it (297 MB, 15.9–18.9 ms).
- **`TCP_NODELAY`:** 400 pipelined pings take 2.2–2.5 ms with it, 42–43 ms
  in two of three runs without; 64 loopback pipelines get 2.9–3.0 million
  answers in 14 s, against 4.9 million without, at the same probe latency.
- **16 MiB upstream answer cap** (`MAX_RESPONSE`, until S6's 1 MiB, design
  §4): a 16 MB answer costs a 7.2 ms step for its two whole copies, flagged
  as `copy=16000016/1048576`; any answer over 1 MiB breaks the copy bound.
- **Repeated list headers:** `field()` gives a list's first line (Accept is
  the only list). A split Accept is served, but a tool call streams only if
  the first line lists `text/event-stream`. No cost measured.
