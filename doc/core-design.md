# Daemon core design

Design for the threaded, asynchronous HTTPS core of the daemon, to be built before the semantic engine. Bracketed tags cite measurements and code reviews made while designing it (2026-10-09): [M] mcp2210-proxy's threading, [T] twinception/askai under load, [S] srtview's stalls under load, [P] platform and protocol probes, [ws] a work-sharing simulation, [J1]/[J2] the design judges' re-measurements. The experiments themselves were scratch work and are not in the repository.

## Summary

**Yes: the core can and should be finished before the semantic engine is designed.** The engine needs five things from the core, and none of them depends on the engine's design:
1. a bounded CPU step (`step(task, budget) → DONE | MORE`) with placement;
2. a blocker pool for files and sqlite;
3. supervised git children;
4. an LLM scheduler with classes, fairness, cancellation and progress;
5. a stutter harness.

The core builds all five, and askai's `tools/call` and a synthetic task are their first users. Building the engine first would repeat srtview, where semantic work grew on the thread that serves clients.

**In the core:** N loops (N = logical cores) plus a brainstem (the main thread), a logger and two blocker threads; lock-free per-loop inboxes and generation handles; HTTPS (OpenSSL 3.6) with HTTP/1.1, chunked, SSE, limits and deadlines behind a stream interface; MCP Streamable HTTP in every era askai supports; the LLM scheduler; tasks, git children, timers; `struct buf`; non-blocking logging, metrics, watchdog, shutdown, reload; the load harness gating CI.

**Not in the core:** the semantic archive, search, prompts and schemas; HTTP/2 and the MCP tasks extension (next); WebSockets, io_uring, HTTP/3; kTLS (no CONFIG_TLS here [P]); SSE resumption (removed in 2026-07-28 [P]).

**Why no srtview stutter.** srtview stalled on O(state) work per event on its serving thread (a 3.4 s refold per landing, an O(N) peek per offer), uncoalesced pokes, a mutex held across file I/O, and one non-streamed LLM lane [S]; mcp2210-proxy on lifetime and policy bugs [M]. Here loop work is only bounded steps under a per-turn budget; loops never block, lock, touch files or scan whole state; wakeups coalesce; admission bounds every queue; the LLM stream has an idle timeout and fair queuing. Each step counts its work in units with fixed bounds, and CI fails when a step exceeds one, when a client is starved while others are served, or on errors, lockouts and leaks (§8); latency is reported, and judged only against the same machine's idle baseline.

**Basis.** The judges split 93 to 90. The base is the winner on points, evolve-existing (in-place loop.c/httpd.c, level-triggered, one load-aware acceptor, fd BIO, staged gates). Its four-way-guarded shared job had a use-after-free [J2], so the ownership model comes from symmetric-loops: lock-free inbox, generation handles, one completion per job. Further grafts: a turn that always runs one bulk step, sliced escaping, the EMFILE spare fd, a per-principal connection cap, a separate logger (symmetric); an open-loop probe, nice-0 helpers, task re-placement at slice boundaries, progress as state (work-sharing).

## 1. Thread model

| Thread | Owns | Blocks in |
|---|---|---|
| brainstem (main) | signalfd, process lifecycle (start, shutdown), watchdog, resource monitor; no client work (§11.1) | `ppoll` |
| loop 1..N-1 | its connections for life (fd, `SSL*`, bufs, parser, call slots, timers), its tasks, its log ring | `epoll_pwait2` |
| loop 0, home | listener, LLM scheduler + curl multi, job registry, children, `SSL_CTX`/config; no clients when N > 2 | `epoll_pwait2` |
| logger ×1 | stderr | `poll`, `writev` |
| blocker ×2 | posted file and DNS closures | file syscalls, `getaddrinfo` |

**N** = CPU_COUNT(sched_getaffinity), capped by cgroup `cpu.max`.

**Extra threads** exist only for APIs that can only block, and sleep in the kernel, so runnable threads stay at N. The logger exists because stderr's O_NONBLOCK is shared with the parent (askai froze 115 s in fprintf [T]); the blockers because regular files ignore O_NONBLOCK (two, so a slow fsync never delays a cert reload). The brainstem wakes only for signals, its watchdog's looks (every half threshold, 10 ms by default) and a resource sample a second.

*As built in S3* (N = 1, §11.1): five threads. The brainstem is the main thread; one loop thread (`loop0`) serves the clients and does home's work, the listener and the curl multi included; the logger and two blockers sleep until there is work. Every thread is started with all asynchronous signals blocked and named for `top`. The logger and the blockers are detached: each counts itself out with an atomic and writes the brainstem's eventfd, which therefore stays open until the process exits. The loop thread is joined, so its library state is gone before `exit()` runs OpenSSL's cleanup; a forced exit detaches it instead (review of S3: a finished thread left unjoined at `_exit` is a ThreadSanitizer report). Before it starts any, the brainstem grows the descriptor table to 65 536 (or the limit): once threads share it, the kernel waits for an RCU grace period each time it grows, 7–11 ms per doubling here, in whichever `accept` or `socket` call needed the descriptor; the first S3 campaign saw these as steps of up to 15.5 ms. The table costs the kernel 512 KiB; past 65 536 descriptors each doubling still waits.

**Settings:** nice 0 (nice-19 SCHED_BATCH took 1.82 s for a 5 ms task under a hog [ws]); no pinning (pinned p99.9 0.38–1.7 ms [P]); timer slack 1 ns (overshoot 53 → 4 µs [P]); a blocker fills `CURLOPT_RESOLVE`, so libcurl spawns no resolver threads (*as built in S3*: one lookup per host at a time, its addresses serving new requests for 60 s, a failed lookup leaving the old ones; libcurl still looks up a proxy it takes from the environment).

**Rules:**
- **R1.** Every mutable object has one owner thread. Connections never migrate.
- **R2.** Per-loop objects cross threads only as a `struct handle`, which is dropped if its generation is stale. mcp2210 wrote to reused eventfds 1767 times in 0.37 s [M].
- **R3.** Posting transfers everything a message points to, including json-c trees, whose refcounts are non-atomic [T].
- **R4.** Besides single-writer gauges and metrics, SPSC log rings and the global body budget, `struct job` is the only shared mutable object: each field has one writer, with one atomic hand-off word per direction.
- **R5.** Immutable data (config, `SSL_CTX`, `mcp_static`) is published before start, or swapped by message and refcounted.
- **R6.** Every job ends in exactly one completion.
- **R7.** No lock is held across a syscall or O(n) work. The only lock is the blocker queue's.

```c
/** Cross-thread name of a per-loop object: never a pointer, never an fd. */
struct handle {
	uint64_t bits; /**< loop:8 | slot:24 | gen:32 */
};
```

**Turn.** Callbacks only flag and enqueue. Each turn: `epoll_pwait2` (≤ 128 events, zero timeout if work is pending); ≤ 256 inbox handlers (the rest wait on a local list); ≤ 64 timers; interactive ready-list steps round-robin until 500 µs; at least one bulk step, more until 1 ms; reap. A **step** is one recv/`SSL_read` (≤ 64 KiB) plus ≤ 8 cheap requests (or 100 µs), one TLS flight, one 16 KiB JSON parse slice, one 64 KiB result-escape slice (escaping costs 18 ms per 4 MiB [T]), one write, or one ≤ 250 µs task slice. Steps are timed per kind; one over 250 µs is an overrun (asserted in debug builds). A turn thus stays near 1 ms plus one overrun, under the 2 ms invariant.

*From S2b* (§8, §11.5) the bound is checked in units of work, not time, since a busy machine stretches any step: `loop.h` names them with their bounds per step (requests 8; bytes received 64 KiB, sent or queued by an application 256 KiB, of body taken in 64 KiB, of JSON parsed 16 KiB, escaped 64 KiB, copied whole 1 MiB; JSON values parsed or freed 256; accepts 32; from the review of S3, closes 32), steps count what they do and stop at the bound, and the statistics keep each unit's most in a step, with the most events (≤ 128), timers (≤ 64) and steps in a turn. The time budgets above still order the turn. Work that cannot be split counts in full where it happens, so the statistics can show a bound broken: a library's copy (json-c copies a string whole when it ends), an answer's text read whole, an application's `data()`. The review of S2b found the first version counted only work it had already capped, so no step could ever show over a bound.

## 2. Messaging

The only primitive: a lock-free intrusive inbox per loop plus one counter-mode eventfd. Producers push with a Treiber CAS; the consumer takes the whole list with one exchange, which rules out ABA [J2].

```c
struct msg {
	struct msg *next;
	void      (*fn) (struct loop *loop,
	                 struct msg  *msg);
};

static inline void
loop_post (struct loop *dst,
           struct msg  *msg)
{
	struct msg *head = atomic_load_explicit(&dst->inbox, memory_order_relaxed);
	do msg->next = head;
	while (!atomic_compare_exchange_weak(&dst->inbox, &head, msg));
	if (atomic_exchange(&dst->asleep, false))
		(void)eventfd_write(dst->wake_fd, 1);
}
```

**Sleeping.** Before blocking, the loop stores `asleep = true`, then loads `inbox`, and does not block if it is non-empty. All four accesses are seq_cst, so either the producer sees `asleep` or the loop sees the message. After waking it clears `asleep` and reads the eventfd only if epoll reported it [J1]. The taken list is reversed, keeping per-producer order. A gated push costs 96–115 ns against 460–486 ns with a write per push [P]; EFD_SEMAPHORE took 179 µs to drain 512 posts [M].

**Embedded messages.** A message continuing an exchange (progress, done, cancel) lives in its object beside a pending word. The poster does `fetch_or(bit)` and, only on 0 → non-zero, takes a reference and pushes. The handler unlinks the message, exchanges the word to 0, acts on the bits, and drops the reference. "Queued" and "what happened" are one atomic, so they cannot race; a burst is one message (srtview's poke storm [S]); posting cannot fail, so R6 holds under OOM [J2]. Messages that open an exchange (new connection, status query) are allocated; failure is a 503.

**Bounds and priority.** Inbox length is bounded by live objects, which admission bounds. Priority lives in turn order and the scheduler, so status, ping and cancel never wait behind the LLM. The blocker queue is `mtx_t` + `cnd_t` + a list capped at 256; full fails the request.

*As built in S3* (`loop.h`, `blocker.h`): the inbox as above, each message stamped at its post for the inbox delay metric, and the turn taking it after its events and before its timers. Its first users are blocker completions and the brainstem's orders: drain, statistics and the test stall, bits of one embedded message. The blocker queue takes POSIX's mutex and condition variable, which glibc's `mtx_t` and `cnd_t` wrap: ThreadSanitizer does not see the wrappers, and took the queue's own fields for races.

## 3. Network layer

**Accept.** Home accepts up to 32 connections per turn on one level-triggered listener and posts each to the lighter of two random loops among 1..N-1 (weight = connections + util‰/4, counted at assignment so bursts don't herd). EPOLLEXCLUSIVE sent 98.7 % of a trickle to loop 0 [J2]; SO_REUSEPORT never rebalances [P]. A spare fd absorbs EMFILE. Accept never pauses for idle clients (a client waited 62.7 s [T]).

**Events: level-triggered**, with loop.c's MOD elision. LT costs 2.07 syscalls per request, ET 3.03 [P]. While a job runs or output is above high water, a connection watches only EPOLLRDHUP (`conn_rearm`, httpd.c:436-445), so it cannot spin [J1].

**TLS: OpenSSL 3.6**: ECDSA handshakes ~200 µs (RSA ~700), 14× on 16 threads, 21.6 KB idle with `RELEASE_BUFFERS`; GnuTLS lacks ML-KEM, mbedTLS is ~5× slower [P].
- TLS 1.3, ECDSA or Ed25519 cert; `RELEASE_BUFFERS | NO_AUTO_CHAIN | ENABLE_PARTIAL_WRITE | ACCEPT_MOVING_WRITE_BUFFER`; one ticket, no session cache; X25519MLKEM768; ALPN `http/1.1`.
- fd BIO with `read_ahead=1` (without it, two reads per record [P]); `SSL_has_pending` feeds the ready list.
- Handshakes are bulk steps on the owning loop (first flight 165–219 µs, second 35 µs [P]); placement spreads storms, ~0.65 ms per loop for 100 clients. RSA (659 µs first flight) needs `tls.allow_rsa`.

**HTTP/1.1.** Kept from httpd.c: resumable `head_length`, `parse_head`, `tchar`/`has_token`, constant-time auth, Origin defence, 100-continue, CORS/PNA [T]. New:
- parsing in place in `struct buf`, consumed by offset (a 1.12 s pipelining stall fell to 97 ms [T]);
- [M]'s parser contract: 0, ENODATA plus an offset, or an error that closes only that connection;
- chunked request bodies, which RFC 9112 requires [P];
- optimistic writes, arming EPOLLOUT only on EAGAIN; headers from fragments (printf cost 3–5 % [T]);
- `MSG_NOSIGNAL`, with SIGPIPE ignored [M].

**Stream interface** (S2): MCP sees only this, so h2 via nghttp2 inherits no HTTP/1.1 assumptions.

```c
struct stream_ops {
	int         (*head)    (struct stream *s,
	                        uint16_t       status,
	                        struct span    fields,
	                        size_t         length); /**< or STREAM_OPEN */
	int         (*data)    (struct stream *s,
	                        struct span    bytes);  /**< EAGAIN above high water */
	void        (*end)     (struct stream *s,
	                        int            error);
	void        (*waiting) (struct stream *s);      /**< a FIN now cancels */
	struct span (*field)   (struct stream const *s,
	                        size_t               i);
};
```

Upward: `on_head` (method, body length or `STREAM_OPEN`), `on_body` (decoded bytes, the last piece marked), `on_writable`, `on_cancel` (close, RDHUP, RST_STREAM). No upcall comes from within a downcall, so the application answers from any upcall or from its own steps; `on_cancel` never follows `end`. **SSE** is chunked `text/event-stream` with `X-Accel-Buffering: no`, id-less `data:` frames, and a `:` keep-alive every 15 s, under the 60 s SDK and nginx timeouts [P].

*As built in S2b* (`src/stream.h`), three things differ from the first sketch, each because HTTP/1.1 needs it and h2 does not mind:
- `head` takes the body's length: h1 frames a known length with Content-Length and an open one (`STREAM_OPEN`) in chunks, or for an HTTP/1.0 client until the close.
- The application names the request fields it reads, and `field` returns them, so the transport keeps their offsets and MCP parses no header.
- `waiting` marks a response that waits outside the loop, such as an upstream call. Only from then on does h1 take a half-close (RDHUP) as cancellation. Otherwise the step budget could make cancellation depend on timing: a cheap request deferred to a bulk step would be dropped for a client that pipelined and half-closed, where S2a answered it. In h2 a half-close ends every request, and only RST_STREAM cancels.

Settled in the review of S2b:
- **Output limit.** `data` may take the pending output past `out_max` only while the response is all of it and has not been told to wait (EAGAIN); otherwise such a write ends the stream and closes the connection. A response therefore goes out in pieces of any total size as the client reads, and a client that leaves pipelined responses unread is still cut off. The first version applied the limit to a response's own head and chunk terminator, which were separate writes, so it dropped every tool answer over 1 MiB.
- **Pieces.** MCP sends a reply a piece a step, each at most the step's share of sending (256 KiB), and after EAGAIN waits for `on_writable`. HTTP/1.1 counts the bytes an application queues as sent work.
- **Lists.** An application marks the fields that are lists (RFC 9110 5.3), such as Accept: they may come in several lines, and `field` gives the first. Every other field may come only once (400). A split Accept was refused before.
- **`on_writable`.** A response that ends there gets the next step, which takes up the connection's next request. Before, a connection whose response ended there with no output pending was left without events, step or deadline.

The application copies a body as it arrives, one memcpy per byte, so the transport can cut it out of its input and keep only the head; chunks are decoded in place, with CRLF-only framing and validated trailer fields. SSE answers `tools/call` when the client accepts `text/event-stream` and its era has Streamable HTTP: all but 2024-11-05, a legacy request without `MCP-Protocol-Version` counting as 2025-03-26, as the 2025-06-18 revision prescribes. The stream opens when the call starts.

**Limits.** Deadlines live on the heap and are never refreshed by single bytes.

| Limit | Default | Breach |
|---|---|---|
| head | 16 KiB, 64 fields, 10 s | 431 |
| body (per tool) | tunable, default 1 MiB; ≥ 4 KiB/s after 5 s | 413 |
| JSON (per tool) | depth 32, ≤ 1024 values, ≤ 1024 members per object (S2b), numbers ≤ 1 KiB (S2b review) | 400 |
| bodies in flight | 256 MiB global | 503 |
| idle keep-alive | 30 s, LRU eviction under fd pressure | close |
| per principal | 256 connections | refuse |
| pending output | pause at 256 KiB, close at 1 MiB unless one response is all of it | close |
| write stall | 30 s | close |

Connections are capped only by RLIMIT_NOFILE. These limits end three [T] problems: the slow-loris lockout, 830 MB of idle retention, and 585 MB from one body.

**WebSockets: no.** Streamable HTTP is the MCP standard every target client speaks, and in 2026-07-28 closing a request's stream *is* cancellation [P]. WebSockets would add a second framing, cancel model and auth path (browsers cannot set Authorization on them). Their one real gain, more than 6 streams per origin, comes from h2.

## 4. Jobs

```c
/** LLM call shared by its origin loop (front) and home (back): R4. */
struct job {
	struct msg       to_home;     /**< bits: SUBMIT, CANCEL */
	struct msg       to_front;    /**< bits: PROGRESS, DONE */
	struct list      hook;        /**< home only: principal FIFO */
	struct buf       request;     /**< front writes before SUBMIT */
	struct buf       result;      /**< home writes before DONE */
	struct handle    call;        /**< origin call slot; stale -> drop */
	uint64_t         principal;
	uint64_t         deadline_ns;
	_Atomic uint64_t progress;    /**< tokens:32 | queue position:32 */
	_Atomic uint32_t home_bits;
	_Atomic uint32_t front_bits;
	_Atomic uint32_t refs;        /**< front + home + each queued message */
	int16_t          status;      /**< home writes before DONE */
	uint8_t          class;       /**< interactive, background */
	bool             preempted;   /**< home only */
};
```

**`tools/call`:**
1. **Submit.** The origin loop validates (incl. base64 `Mcp-Name` [P]), builds the upstream body, allocates the job (refs = 2) and a call slot, posts SUBMIT, opens SSE.
2. **Admit.** Home takes ≤ 8 queued jobs per principal, ≤ 256 in total, only while the breaker is closed; otherwise DONE(busy) at once.
3. **Schedule.** Per-principal FIFOs with deficit round-robin (an [M] flooder pushed others from 1.2 ms to 10.7–40 ms); interactive first; S slots = `-np`. With S ≥ 2 one slot is reserved for interactive work; with S = 1 interactive preempts background, requeued at most once.
4. **Upstream.** Home runs it over http.c's curl bridge [T]:
   - `stream:true`, parsed by sse.c; empty proxy and `Expect:` [S];
   - `MAX_HOST_CONNECTIONS = S`, `MAXCONNECTS = S + 1` (askai kept 3000 idle sockets [T]);
   - deadline from dispatch plus a 60 s per-chunk idle limit (today's timeout counts queue time [T]; srtview's 3600 s held its lane [S]);
   - a 1 MiB answer cap, consumed ≤ 64 KiB per turn via `CURL_WRITEFUNC_PAUSE` (an 8 MB answer stalled the loop 29 ms [T]);
   - errors fail only their transfer (http.c:86 [T]); a breaker on refusals, timeouts, 5xx and stalls, half-open probing [S].
5. **Progress and done.** Home updates `progress` and sets PROGRESS; the front emits at most one `notifications/progress` per 100 ms. On DONE, the front looks up `call` and replies, or drops the result if the call is stale.

**Cancellation.** Close, RDHUP, a write error or the output cap frees the call slot (gen++) and sets CANCEL; home dequeues the job or removes its curl handle (5/5 cancelled today [T]) and sends DONE(cancelled). Legacy `notifications/cancelled` goes through home's (session, requestId) registry. RDHUP cancels in every era: without resumption a legacy result is lost anyway [P]. Status and `tasks/get` are one ~5 µs round trip to home [P].

**Tasks** (the engine's interface) are ≤ 250 µs steps on the bulk list, placed by power of two choices at submit and re-placed at a slice boundary when the local backlog exceeds 2 ms (local-only placement diverges past one core of demand [ws]). Unsliceable calls go to the blocker.

**git:** `pidfd_spawn`; pidfd and pipes (into capped bufs) on the task's loop; `waitid(P_PIDFD)`, no SIGCHLD; SIGTERM then SIGKILL via `pidfd_send_signal` at the deadline; a global child cap.

**Timers:** a 4-ary heap per loop, one entry per object, drives `epoll_pwait2`. Activity only stamps `last_active` (coarse clock, 4 ns [P]); an early-firing entry re-inserts itself. curl uses home's heap. This replaces httpd's O(n) tick and timerfds (3.6 µs each [P]).

## 5. Memory and data

```c
/** Binary byte buffer for sockets, TLS and pipes; [head, tail) is readable. */
struct buf {
	unsigned char *data;
	size_t         head;
	size_t         tail;
	size_t         cap;
};

struct span {
	unsigned char *ptr;
	size_t         len;
};

/** At least @p min free bytes at the tail, doubling up to @p max; empty on ENOBUFS. */
extern struct span
buf_reserve (struct buf *buf,
             size_t      min,
             size_t      max);
```

Also `buf_commit`, `buf_peek`, `buf_consume` (resets when empty), `buf_trim` (frees when idle) and `buf_wipe` (secrets only; nothing else zeroes). Compaction happens only when a reserve fails and `head ≥ cap/2`: amortized O(1). `max` is the backpressure signal. recv and `SSL_read` fill the reserved span directly [T]. This replaces mcp2210's buf (linear growth, zeroing, memmove per consume [M]) and dstr on the I/O path (UTF-8-only, 255 reallocs to reach 4 MiB [T]); dstr keeps text.

**No pool at first.** Buffers are trimmed when idle and after any request over 64 KiB (prevents [T]'s 830 MB); `mallopt(M_MMAP_THRESHOLD, 128 KiB)`. An idle connection costs ~300 B, ~22 KB with TLS [T][P].

**JSON.** json-c and jb.c stay, with one reused tokener per loop (per-parse locale setup cost 2 % [T]).
- A streaming pre-scanner counts values and depth outside strings. 1024 values parse in ~95 µs and free in ~30 µs [J1]; cost tracks values, not bytes (4 MiB of strings: 5 ms; of small objects: 278 ms + 103 ms free [T]). Bodies over 16 KiB parse in slices.
- mcp.c splits into an immutable `mcp_static` (pre-serialized tools/list, discover, initialize, serverInfo `_meta` bytes) and per-loop state, removing the shared `json_object_get` sites at mcp.c:173, 368, 423, 488, 491 [T][J2].

*As built in S2b:*
- **Tokeners.** Each loop keeps a pool of up to four reused tokeners. Sequential parses use one, as designed. Two sliced parses cannot share one: a tokener holds its parse's state, and with one per loop every body would wait for another's whole parse. A tokener goes back to the pool only after a parse of one slice, since json-c keeps the buffer of the longest string it has seen; a larger parse frees its tokener, ~1 µs against two or more slices.
- **Slices.** A slice is at most 16 KiB and 256 values. The scanner finds where it ends, before the value that would be one too many, or before a split character. A body that fits the step's remaining share is parsed at once, so cheap requests are answered in their own step.
- **Members.** The pre-scanner also counts members per object (`--max-members`, 1024). When an object grows, json-c rehashes its whole table in one call: 116 k keys made 11 ms steps, while 1024 members cost under 0.1 ms.
- **Numbers** (review of S2b). json-c converts a number whole when it ends, strtod over all its digits: a 1 MB number took a 1.9 ms step, 16 MB 16 ms. The pre-scanner limits numbers and literals to 1 KiB (400), and so does the parse of an answer; no valid one needs more.
- **Copies** (review of S2b). json-c also copies a string whole when it ends, about 0.4–0.5 ms per MiB, which no slice can split. It counts in full as `copy` work, bound 1 MiB, which the default body limit keeps to; a raised `--max-body` shows in the statistics as a step over the bound.
- **Answers** (review of S2b). The upstream's answer is parsed like a body, a slice a step, and checked to be UTF-8 slice by slice, since json-c lets overlong forms through; its text is read in a step of its own, and the tree freed in slices. A 16 MB answer took one 36 ms step; now its two whole copies, json-c's and the text's, take a few ms each. Answers stay capped at 16 MiB until S6's 1 MiB cap (§4), so one over 1 MiB still breaks the copy bound, and the statistics show it.
- **Freeing.** Freeing is sliced too: 256 values a bulk step, each container taken apart from its last element or first member in O(1). Each request frees its own message after its reply has gone out, and ends its stream only then. Its connection's next request waits, so message memory is bounded by the requests under way. With one shared freeing entry, several dense clients outran it: RSS reached 2.3 GB. A parse that fails or loses its client hands json-c's unfinished containers to the same slices. This reads two tokener fields json-c documents as private.
- **Memory.** glibc keeps freed small chunks, so askai calls `malloc_trim(0)` when a turn is quiet, checked every 5 s. The call can be neither split nor moved off the loop. Its cost is the release of pages, about 50 µs per MB. Trimming after every few hundred values freed cost up to 1.6 ms a call on the heap dense bodies leave, as each call walks its ~5000 free chunks again. A trimming thread would hold the loop's arena lock as long. So after a burst of large trees the trim takes one step: 16–22 ms after the dense and check profiles, which free 280–330 MB, and 53–67 ms beside 32 CPU hogs. Since the review of S2b the harness shows that step (§8), within the gross-outlier limit. jemalloc with background purging released the memory off the loop, but kept about 5 MB more and had turns of 9–11 ms under load; mimalloc purged in a 19.8 ms step. The allocator stays open (risk 6). json-c takes ~0.65 KiB for an empty object: raising `--max-values` raises memory per request accordingly.
- **`mcp_static`** came in S2b rather than S4. Replies are built from fragments: the static ones, and per-request ids, messages and answers, escaped 64 KiB a step. The upstream request deep-copies what it takes of the configuration (temperature, `--extra`), where it took references at first: their counts are not atomic, and with N loops two requests would race on them (review of S2b).

## 6. Failure, shutdown, logging, metrics

**Isolation.** Callbacks return `void`, and an error closes only its own connection or job. `loop_fatal()` is for broken invariants; today any callback or curl error ends the daemon (loop.c:146-147, http.c:86 [T]). Out-of-memory gets a 503.

**Shutdown.** Signals are blocked before `pthread_create`; the brainstem reads a signalfd. Thread start is a monotonic-clock handshake that propagates errors [M]. On SIGTERM, home closes the listener and posts each loop's pre-allocated `drain`; loops close idle connections, cancel queued jobs, end SSE with an error event and give running work 5 s; the brainstem joins the loops, the blockers, then the logger. A second signal exits at once. No `pthread_cancel`.

*As built in S3* (`brainstem.h`): before any thread starts, the brainstem blocks every asynchronous signal, so threads inherit the mask; it then takes SIGINT, SIGTERM, SIGHUP, SIGUSR1 (statistics) and, with `--test-stall`, SIGUSR2 through its signalfd, and leaves the rest to their default actions on itself. Of the first three, one that the process started with ignored, as nohup and a shell's background job leave them, stays ignored and out of the signalfd, which would take it all the same: Linux keeps a blocked signal pending whatever its action (review of S3: `nohup askai` ended on SIGHUP). Each thread reports its start, or its error, through an atomic word and the brainstem's eventfd, which the brainstem waits on with `ppoll` against a monotonic deadline (10 s); it never takes a lock a loop holds nor waits for a loop. The first SIGINT, SIGTERM or SIGHUP (SIGHUP until S5's reload) orders the loop to drain within the grace period (`--grace`, 5 s):
- the listener closes and idle connections close; a request under way finishes, its body included, and its response says `Connection: close`; a connection between requests ends rather than begin another, one part way through a head when it next sends or at the deadline;
- a tool call waiting for the upstream, or not yet sent there, ends at once with a JSON-RPC error, on an event stream as its last event, and its upstream request is cancelled. Giving it the grace period would rarely let it finish, as answers take seconds to minutes; the client learns at once. S6's scheduler may refine this with progress;
- connections left at four fifths of the grace period close; the loop exits once none is left.

The order to drain only flags and queues: bulk steps close idle connections, and at the deadline every one left, 32 a step (`closes`), and end tool calls 8 a step (`reqs`). The first version did all of it in the order's handler, one turn for the whole drain: 50–100 ms for 10 000 idle connections, 250–500 ms for 40 000 (review of S3).

The brainstem then joins the loop, stops the blockers (2 s at most: a blocker stuck in a call is left to the process's exit, its loop kept for it), and gives the logger 1 s to write what it holds; exit status 0. A second signal, the grace period running out, or the loop found in one turn for 1 s while it should drain, ends the process at once with status 3, after the logger has had up to 1 s; signals then go unread. Teardown after the loop's last turn is no turn to the watchdog, which saw a slow `curl_multi_cleanup` as a stuck loop (review of S3).

**Reload.** On SIGHUP a blocker reads the files and home posts a new `SSL_CTX` and config; old ones live on by refcount.

**Logging.** Per-thread 256 KiB SPSC rings of preformatted records; a full ring drops and counts. Per-call-site rate limits; disabled levels skip argument evaluation [S]. The logger is woken through the same asleep-gated eventfd and is the only thread that blocks on stderr (`writev`).

*As built in S3* (`log.h`): `LOG(level, …)` formats a whole line of up to 1 KiB on the caller's stack and copies it into the caller's ring, or drops it and counts it; a call site logs at most 32 lines a second, and its next line says how many it held back. `-v` enables the verbose level; levels above it skip the call. The logger writes the rings round-robin with one `writev`, waits in `poll` for room on a non-blocking stderr, and blocks on a blocking one; it reports drops once it can write again. It is detached, and once it says it is done it touches nothing shared but the brainstem's eventfd, so no exit path waits for it beyond its bound. A thread that finds no ring left drops its lines, counted, rather than write stderr itself; the brainstem sizes the rings by its threads. Every line askai wrote to stderr goes through it, but for errors in the options, before it starts.

**Metrics.** Per-thread log-linear histograms (relaxed atomics) at `GET /metrics` (loopback or token) and a `status` tool: turn and step time, inbox delay, timer lateness, per-method latency, handshake CPU, job wait / time to first token / run time, drops, MODs.

*As built in S3* (`metrics.h`, `hist.h`): every histogram and gauge has one writer, which updates it with relaxed loads and stores, as cheap as plain ones, and so do most counters; those of log lines and blocker jobs, which several threads add to, are atomic sums of rare events; readers use relaxed loads. `GET /metrics` gives Prometheus's text format: per loop, turn and step times, timer lateness, inbox delay and the most of each unit of work a step did; request times by MCP method; upstream wait (its host lookup included) and run times; host lookups; blocker jobs; log lines, drops and held-back lines; watchdog reports and late wakes; resident memory, descriptors and their limit, and PSI where the kernel has it. Histograms have buckets from 1 µs to 300 s. A request renders at most 32 cost units a bulk step, a histogram costing 8, then answers with the whole text (about 26 KB). The transport marks a stream trusted when the client showed the token or, none being set, is on loopback and names no other host than localhost, 127.0.0.1 or [::1] in `Host`; others get 403. A page whose name an attacker rebinds to 127.0.0.1 sends no Origin with a same-origin GET, but names its own host (review of S3). The `status` tool, handshake CPU (S7), time to first token (S6) and MOD counts are not built yet. The SIGUSR1 statistics are unchanged: windows between reports, kept beside the metrics.

**Watchdog.** It runs on the logger rather than home, since home is the likeliest to stall [J2]. It flags any loop that has been inside one turn for more than 20 ms (`turn_start` set, not asleep), unless its own wake was late [S].

*As built in S3*, it runs on the brainstem (§11.1), which, unlike the logger, never blocks on stderr. Each loop stores, with relaxed atomics, when its turn began, and 0 while it waits for events. Every half threshold (`--watchdog`, 20 ms) the brainstem reports a turn longer than the threshold, once per turn, through a rate-limited line and a counter: a diagnostic, not a gate. Woken more than a threshold late, it was held up itself, likely with the rest of the process or the machine, so it vouches only for the time since then: SIGSTOP of the whole process, as the harness's stall check does, reports nothing. Its own resource sample does not count as lateness: counting 100 000 descriptors took tens of ms, which made every second's look late, so that a loop stuck while draining ran into the grace period instead (review of S3). The sample reads `/proc/self/fd` with `getdents64` into a buffer on its stack, as `opendir` would `malloc` in the main arena, whose lock the loop's `malloc_trim` takes.

## 7. Kept, adapted, new

| Source | Verdict |
|---|---|
| [T] loop.c `loop_ref`, `loop_release`/`loop_reap`, MOD elision, LT | keep; add inbox, heap, lists, budgets, `epoll_pwait2`, void callbacks |
| [T] httpd.c parser, auth, Origin, 100-continue, CORS/PNA, `conn_rearm` | adapt onto buf/streams; drop MAX_CONNS, listener pause, srv_tick, dstr I/O, printf |
| [T] mcp.c eras, validation, cancel; jb.c; sse.c; `serve()` | keep; add `mcp_static`, base64 names, progress, SSE |
| [T] http.c curl bridge | adapt: host caps, streaming, pause budget, heap timers |
| [M] owner per resource, intrusive hooks, `list_transplant`, parser contract, start handshake, buf API shape, load.c/stub | keep the patterns |
| [M] evfd | adapt: counter mode, one per loop, `asleep` gate |
| [M] skip mask, EFD_SEMAPHORE, per-conn eventfds, raw-fd targets, shared FIFO, unbounded queues, `pthread_cancel` | drop |
| new | buf, inbox, handles, job, heap, steps, TLS, SSE, streams, scheduler, tasks, children, log rings, metrics, harness |

## 8. Proof: bounded work, shared degradation

**Policy** (from the user, at S2b). The daemon will not run on a brand-new number cruncher. It will share busy or slow machines, and when the machine struggles, its connections should share the struggle. So no absolute latency target is a gate. Absolute latencies are measured and reported as information. A run fails only on:
- errors;
- lockouts;
- leaks;
- limits or deadlines not enforced;
- unfairness;
- unbounded work;
- gross outliers relative to the same machine's idle baseline, measured in the same run.

Bounded work per turn stays mandatory, but it is checked in units of work (§1).

The reasons were plain in S2a's records. An absolute bound measures the machine as much as the daemon: S2a's one turn over 1 ms in nineteen storms came from the desktop preempting askai, while askai's own steps stayed under 0.65 ms. A fast machine can also meet a latency target while one client starves, which is the failure that matters to users. S0's lockout, where every fresh connection timed out while the others answered in 37 µs, is such a starvation. So is S2a's review finding of step-queued work waiting 5 s behind callback work.

**Harness** (`tests/load/`, C, grown from [T] probe.c and [M] load.c):
- an open-loop Poisson probe (ping, tools/list at 1 kHz) timed from the *scheduled* send, so coordinated omission cannot hide stalls [ws]; its keep-alive connections take the arrivals in turn, each queueing its own, so one held back builds up latency rather than shedding arrivals to the others;
- modes idle, jobs, loris, slow body, slow reader, pipeline, malformed, churn and hostile JSON; from S2b also dense JSON (1 MiB bodies of many small values), tool calls that hang up on their event stream, and large answers (512 KiB); the heavy modes (pipeline, malformed, hostile, dense) time each batch or body;
- a C upstream mock with slots, token rate, SSE and cancel counting;
- ws.c in `tests/bench/`.

A run has up to four phases on one daemon: the probe alone, as the idle baseline; the heavy modes alone, a few rounds each, as theirs; the profile's load with the probe; and the probe alone again, for as long as the load, while the daemon gives the load's memory back.

| Clause | A run fails on |
|---|---|
| errors | a crash or non-zero exit of the daemon, generator or mock; a probe arrival failed, timed out or lost, in any phase; a background request unanswered; a tool call failed or cut off; mock counters that do not add up |
| lockout | a fresh connection not served (fresh-connection probe, churn) |
| limits | loris and slow-body clients open past their deadline; an upstream call outliving its client |
| leaks | descriptors not back at idle + 2, or RSS not within 4 MiB of idle, by the end of the after phase |
| fairness | a probe connection's p90 over 10× the median connection's; a probe kind's p90 over 10× the median kind's; a tool-call connection with no call done while the median one did 3; a heavy mode's p90 over 50× its p90 alone, or none done |
| work | a unit of work in a step over its bound, unsplittable work counting in full (§1) |
| outliers | probe p99 in the load or the after phase over 100× the idle p99; a turn in either over 1000× the idle p99 turn and over 100× the idle probe p99 |

Fairness uses the p90 because a starved client is slow throughout, while a machine hiccup hits every client alike and reaches one client's p99 only when a few land on it. All the factors are generous on purpose. A starved client shows thousands of times its peers' latency (S0's lockout: 5 s against 37 µs), while sharing shows 1–2× (S2b storm: 1.1–1.2× on connections, 1.6–1.7× on kinds). Heavy work yields to cheap requests by design, so its factor is the larger; it shows 1.0–10.4× its time alone. The factors are environment variables of `run.sh`.

*From the review of S2b* (`doc/core-measurements.md`), the harness itself was found to hide two things. The fairness check left out probe connections with under 50 samples, and a connection held back took fewer arrivals, so it was the one left out: one answering in 300 ms beside peers at 75 µs passed. And the loop's statistics were taken before a trim that followed the load, 14–19 ms after dense bodies. Hence the per-connection turns, the solo and after phases, and the counting of unsplittable work in full.

**Reported, not judged:** probe p50/p99/p99.9/max and their ratios to the idle phase; turn and step times; timer lateness; inbox delay, in `/metrics` from S3. The former targets remain a reference for a quiet modern desktop, and S0–S2a were judged by them:
- cheap-request RTT p99 ≤ 250 µs, p99.9 ≤ 1 ms, max ≤ 5 ms;
- turn p99.9 ≤ 1 ms, max ≤ 2 ms;
- inbox and timer p99 ≤ 20 / 50 µs.

One loop already reaches p99 37 µs, with max under 90 µs at 3000 calls in flight [T].

**Storm run** (all at once; NDEBUG, unpinned, worst of 3): 10k idle connections; 1000 tool calls from 250 principals on 1 slot; 64 each of loris, slow-body and slow-reader clients; a 4 MiB pipeline; 1000 connections/s of churn; from S7, 200 TLS handshakes/s; from S8, saturating tasks.

**Gates** beyond the table: submitted = completed + cancelled + rejected; upstream cancels = disconnects; stalled stderr harmless (S3).

**CI.** `make check` includes `check-load`: the verdict on a light mix of every mode, 2 s idle, the heavy modes alone, 5 s of load and 5 s after, about 15 s in all (17–19 s on a loaded machine). Since the review of S2b it passed the campaign's five runs on a busy desktop and six beside 32 CPU hogs (load average up to 39). Beside the hogs the trim after the load took 53–67 ms, within the outlier limit only through its arm of 100× the idle probe p99, which the hogs slowed as well. `make load` runs the storm under the same verdict. ASan/UBSan run `tests/askai.py`, and TSan from S3, when threads came (it found mcp2210's races in under 2 s [M]); each also runs a load profile.

## 9. Build plan

Each stage keeps askai and `tests/askai.py` green.

| Stage | Work | Gate |
|---|---|---|
| S0 | harness, mock, baseline | recorded |
| S1 | `struct buf` in httpd: offset consume, recv into tail, trim, optimistic send, fragments; no cap; LRU | 4 MiB pipeline delays others < 2 ms |
| S2a | loop v2: heap, `epoll_pwait2`, lists, steps, void callbacks; limits | loris closed; no turn > 1 ms |
| S2b | JSON pre-scan and slices, stream interface, chunked, SSE, `mcp_static`, work units | dense JSON in bounded slices, others degraded only in proportion; SSE calls cancel on close; §8 verdict on every profile |
| S3 | brainstem (signals, start, shutdown, watchdog, resources), loop thread, logger, blockers and the inbox, metrics | stalled stderr harmless; an injected stall reported; signals end a stuck loop within the grace period; `/metrics` access-controlled; blocker completions; TSan clean |
| S4 | handles, job, N = 1 | TSan clean under churn |
| S5 | N loops, placement, shutdown, reload | storm verdict; ratios to idle no worse than one loop's |
| S6 | scheduler, streaming upstream, breaker, progress | a flooder leaves interactive clients a fair share; cancels match |
| S7 | OpenSSL, handshake steps | TLS storm verdict |
| S8 | tasks, placement, pidfd children | saturated verdict; git killed at deadline |

The gates of S0–S2a were the absolute SLOs of their time; from S2b, every gate is §8's verdict plus the stage's own clause.

After the core: h2 via nghttp2, then the MCP tasks extension.

## 10. Risks and open decisions

**Decided.** Busy jobs get `isError: true` with a retry hint (the calling model sees a tool outcome); transport overload gets 503 + Retry-After. RDHUP cancels in every era. With `-np 1`, preemption is on, at most once per job. Hybrid PQ stays on (+55 µs fits a step). LT, the fd BIO, the single acceptor and "no pool" change only on measurement.

**Risks.**
1. Steps are a convention: a slow library call stalls its loop. Overrun histograms and asserts catch it; unproven calls go to the blocker.
2. Placement weights are guesses until S5/S8; fallback is migrating idle keep-alive connections.
3. Home saturation: the scheduler is message-only, so it can move to its own loop.
4. JSON figures are warm best-case [J1]; S2's overrun data sets the cap.
5. Co-located CPU load: an equal-priority hog pushed loop p99 to 2.2 ms [ws]. Options: `CPUWeight=`, or an isolation mode where loops run no tasks. Under §8's policy this is a shared slowdown, not a failure, as long as it stays shared: `check-load` beside 32 hogs saw 13–66× the idle p50 and connection p90 spreads of 1.05–1.19.
6. glibc arenas may fragment, and give freed memory back only in one call that no step can share (§5): 16–22 ms after the 280–330 MB of json-c trees that dense bodies leave, 53–67 ms on a loaded machine. jemalloc with background purging and mimalloc were measured at the review of S2b (§5); neither is adopted, and the choice stays open.
7. More than 256 CPUs overflow the 8-bit loop field.
8. io_uring (+8 %) is blocked by Docker and RHEL [P]; it stays an optional later backend.

## 11. Revisions after review

Notes from the review of this design, to settle at the stages they affect:

1. **Loop 0 carries too much** (settle before S4/S5). Listener, LLM scheduler, job registry, children and configuration may move onto one or two high-importance "sidekick" threads. The main thread should rather be concerned with the hardware and the operating system the service runs on: a brainstem that still catches a Ctrl-C when the cortex has gone fishing. Signal handling, the watchdog and resource monitoring are natural tenants.

   **Resolved for S3.** The main thread is the brainstem (§1, §6): it owns the signalfd, the process's lifecycle (the start handshake, shutdown), the watchdog and a resource monitor (resident memory, descriptors against RLIMIT_NOFILE, PSI where present, sampled each second, exposed as metrics, logged when over or back under a threshold). It never takes a lock a loop holds nor waits for a loop or the logger beyond a bound, so it ends the process when the loop is stuck. The event loop runs on a thread of its own, which the brainstem starts and stops; the logger is the first sidekick thread. **Left for S4/S5:** with N loops, whether the listener, LLM scheduler, job registry, children and configuration stay on loop 0 or move to one or two sidekicks; draining N loops, and reload on SIGHUP (S5); the resource thresholds' effect on admission, which only logs for now. The review of S3 kept the brainstem's periodic work free of `malloc`, and its forced exits free of unread signals and unjoined threads.
2. **Client loops are their own thing** (confirmed): loops that own client connections do not double as the home of shared services.
3. **Request body limit** (S2): 1 MiB is the bare minimum; users may push far more during an information-gathering session. The limit is a tunable that starts at 1 MiB.
4. **Order** (confirmed): the foundation first, S0 onwards.
5. **Latency policy** (from the user, at S2b; binding from then on): absolute latency targets are not gates. Latencies are reported. Runs fail on errors, lockouts, leaks, limits not enforced, unfairness, unbounded work in work units, and gross outliers relative to the same run's idle baseline (§8). The reason is the target machine: shared, busy or slow, where connections should share any struggle rather than some of them meet a number.
