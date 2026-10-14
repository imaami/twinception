# Semantic engine design

Design of askai's semantic engine: the knowledge base that `doc/core-design.md` names as the work the core was built to carry ("the semantic archive, search, prompts and schemas"). It is a first design, for review before any code is written; nothing in it is built. Its ancestor in spirit is srtview's engine (srtview's `DESIGN.md`, `semantic_engine.hpp`, `semantic.cpp`, `refinery.cpp`, `topics.hpp`, `rx.hpp`, `agenda.hpp`, `vault.hpp`); §2 says what carries over and what does not. This revision takes several points from an alternative design written independently for the same task, chief among them that what is said in a conversation never becomes shared knowledge; §2.3 lists what it took and what it did not.

## Summary

**The problem.** Employees ask how something in a large codebase relates to other code and to the world around it: services, data stores, queues, hosts, partner organizations, teams, documents and procedures. The systems are large and talk to dozens of peers inside and outside the organization. The organization's LLM capacity is small, so the usual IDE pattern, an assistant that reads the code afresh for every question, costs too much (§1.2). askai is already the endpoint these questions pass through. The engine makes each answer leave behind knowledge that later questions can use, also questions worded differently: a semantic cache.

**Decisions**, each argued in its section:

1. **Terms are the currency** (§3.2). A term is a name within a scope: its source for names in code, its namespace for the systems around the code, and the whole estate for organizations. askai finds a name in any text by the rules of identifiers, and compiles it into a pattern that `git grep` runs over source text at any revision. This keeps srtview's regular expressions as the currency of knowledge: a pattern is a search key that can be run again, so it finds evidence again after code moves, and names map differently worded questions onto the same knowledge without asking a model.
2. **Claims are the knowledge** (§3.3). A claim relates a term to a term or to a value through one relation of a small, fixed set, in a context such as an environment, with a sentence and the exact lines that show it. askai copies the quoted lines from git itself and refuses a claim whose cited lines do not name the things it relates. Words that a model writes are never evidence, and every claim says how it is supported: observed by a rule, inferred by a model from the source, or stated by a person (§3.9).
3. **Shared knowledge comes from sources and people, never from conversations** (§3.5). What a model writes while it answers someone stays with that person. Shared knowledge comes from jobs that are shown source text and nothing of any conversation, and from people who answer for what they contribute. An answer's citations say which lines to read, and reading them is a job like any other, so research that one question paid for becomes knowledge that every later question can use.
4. **Git holds what cannot be computed again** (§4): terms, claims, verdicts, testimony and findings, in one repository with one writer, as plain files that a person can read, diff and blame, laid out by where writes land so that a commit rewrites a few small trees, and synced to disk before a write is acknowledged. Occurrences, freshness, coverage and indexes are computed again when needed.
5. **Reads come from an immutable view** (§5): an in-memory index of one commit of the knowledge base, built of persistent maps so that a change copies only what it touches, and published to the client loops by reference, as the policy is. Serving what is known touches neither git nor a model.
6. **Mechanical work first, the client's model second, the upstream last** (§6). The server finds candidates mechanically and says what it searched. The client's own model, which knows the asker's context and runs on the asker's hardware, judges what applies. The upstream is spent on research, and on reading that never takes the slot kept for interactive work.
7. **Every model job is content-addressed** (§8). A job's id hashes its kind, its recipe and every byte the model is shown. The model reads a window of a file once for all users, until the window or the recipe changes, and identical jobs in flight are one job.
8. **It fits the core** (§9). One new sidekick loop, the kb loop, owns the repository, the view, the job queue and all of the engine's git work. The tools run on the client loops against the published view, model work goes to home as background jobs, and every step is bounded.

**The smallest useful version** (§13, stages E0 to E4) reads only what answers cite. The knowledge base fills from the research that `ask_ai` calls already do: an answer cites the lines it rests on, askai keeps the answer for its asker alone, and queues one `extract` job over the windows that hold those lines, which sees nothing but the source. The next question, anyone's, starts with what the job found. Freshness, reading on demand, judgments and searches between terms come in later stages.

## A question's path

1. Alice's assistant calls `knowledge` with "what sends files to Nordea?". The view matches the term Nordea, a global organization, which a curator's verdict joined with NDEA, the code's abbreviation for it. No claim is about either yet, so the kb loop looks up their occurrences at the tracked revisions with `git grep`, among them `nordea_sftp_host` at `payments:config/prod/sftp.yaml:4` and a line of `payments:src/settle/upload.go:41`. The result says that nothing is known about what sends to Nordea, and that the knowledge base has read none of the files where it occurs.
2. The assistant calls `ask_ai`, and askai adds that result to the prompt. With `git`, the upstream reads `upload.go` and the CronJob manifest in the `deploy` repository that runs it, and answers, citing `[payments:src/settle/upload.go:38-47]` and `[deploy:k8s/prod/settle-cron.yaml:12-20]`. askai checks the citations, keeps the answer as a finding in Alice's own repository, and queues one `extract` job over the two windows that hold the cited lines. The job is shown those windows and nothing of Alice's question or answer.
3. In the background, the job reads the code and the manifest side by side. It yields "settlement-uploader `sends_to` Nordea", citing the manifest's lines that run settlement-uploader and the lines of `upload.go` that connect to Nordea, and "settlement-uploader `runs_on` prod-batch", citing the manifest alone. askai checks that each claim's cited lines name both sides, gives both claims the context `prod` that the manifest's path has, and commits them. The manifest's lines have the class `configuration`: they schedule the job on prod-batch, which does not show that it ran there. The knowledge base now holds what the code and the manifest say, and records only that their lines were read, not why or for whom.
4. Bob asks his assistant "how do we deliver settlements to the bank?". No term matches, but "settlements" meets "settlement" in the first claim's statement (§6.2 compares words by their first six characters), the word is rare in the base, and the claim ranks first. Bob's assistant answers from the claim and its quoted evidence, at no cost to the upstream, and can tell Bob that a model inferred it from the code and that the schedule comes from configuration.
5. A week later `upload.go` changes. When the kb loop sees the new revision, it finds that the claim's quoted lines changed and marks the claim as changed. Answers that cite the claim say so. The next question that touches it puts a recheck job in the queue. Had the lines stayed as they were, the claim would read "evidence unchanged", never "current": a constant defined in another file could still have changed what they do.

## 1. Problem and constraints

### 1.1 The questions

The questions come from people who work on or around the systems, through their assistants:

- *Locating:* where is the retry policy for partner callbacks configured in production?
- *Relating:* which services write the `ledger_entries` table, and who owns it?
- *Explaining:* what is PSP in this codebase?
- *Impact:* if the `amount_cents` field of `SettlementRecord` changes, what breaks outside this repository?
- *The world around the code:* what happens when Nordea's settlement file is late, and whom do we contact?

Most of these questions are about the relations between named things: symbols, services, stores, channels, hosts, environments, external organizations, teams, documents and procedures. Some of the answers are in the code, some in configuration and documents, and some only in people's heads.

### 1.2 The budget

As an illustration, take a modest deployment where an upstream slot processes prompts at 1,500 tokens per second and generates 30 tokens per second. The conclusions hold within a factor of a few.

- **Reading everything is out of reach.** A million lines of code is about 40 MB, or about 3,300 windows of 12 KiB. A window is about 3,000 tokens of prompt and about 500 tokens of claims, which takes about 2 s plus 17 s, so about 19 s per window. That is about 17 slot-hours per million lines for each version of the prompt. An estate of 20 million lines takes two weeks of one slot. Reading the whole estate is not a goal. Reading follows demand, and starts at the boundaries where code meets the world (§8.3).
- **Keeping up is cheap.** Jobs are keyed by content (§8.2). If 1 % of the files change in a day, reading the changed windows costs about 10 minutes of a slot.
- **The IDE pattern is the expensive one.** An assistant that researches a question reads tens of thousands of tokens over several turns, which is 20 to 60 s of a slot before it writes anything. A few hundred such questions take a slot's whole working day. Questions overlap heavily, so the knowledge base turns a repeated question into a bundle of a few thousand tokens. The client's model reads the bundle, or the upstream reads it in one turn.
- **Keeping conversations private has a price.** Shared knowledge comes only from jobs that read sources (§3.5), so a question that the base cannot answer pays twice: for its research, and then, in the background, for one job that reads what its answer cited, an excerpt of up to 24 KiB, which takes about 20 s of a slot at these rates. That reading is done once for every later question.

### 1.3 What askai already has

- An MCP endpoint with identities, principals and roles. A role grants tools to clients, and its `upstream` member names the tools that the upstream may call in the turns of the role's ask calls.
- `ask_ai` with turns, given `--upstream-tools`: the upstream may call `git`, `man` and the plugins' lent tools as the role's `upstream` member allows, `notes` whenever askai keeps notes, and with those `compact`, `wait` and `timer`, and `background` and `jobs` with a tool that can run in the background.
- The `git` tool: read-only, allowlisted commands in the listed repositories, `grep` among them.
- The `notes` tool: private notebooks of the upstream and of clients, each thread a bare git repository, each note a file, written by `git fast-import` and never changed.
- Home's scheduler: upstream slots, deficit round-robin across principals, an interactive and a background class (background jobs hold at most all slots but one; with one slot an interactive job preempts a running background job, which then runs again from its start), one breaker for the upstream, streamed answers and progress.
- Tasks (CPU work in slices of 250 µs), children (pidfd, deadlines, at most 16 at once in the process), blockers, and the stutter harness, whose verdict fails a run on unfairness, unbounded work and gross outliers.
- Plugins that a configuration file loads. A loaded plugin may start no process or thread and cannot reach home, so the engine is built in, as `man` and `git` are.

### 1.4 Goals

- **G1** A question asked again, in the same or other words, costs no research while the knowledge exists and still holds.
- **G2** Everything served points to evidence that a person can check, and says how it is supported, whether its evidence is unchanged, and what the search behind it covered.
- **G3** Interactive work goes first: background jobs never take the slot kept for interactive work, and take at most `--kb-jobs` others. A job is asked of the model again only after a failure, a preemption or a crash.
- **G4** Knowledge survives restarts, changes of model and prompt, and changes of code, where it finds its evidence again.
- **G5** The engine keeps the core's rules: bounded steps, one owner for each object, no polling, no stutter.
- **G6** A person can read, diff and blame the knowledge base with git.
- **G7** What one person asks, and what a model answers them, reaches nobody else through the knowledge base.

### 1.5 Non-goals

- Complete coverage of an estate (§1.2).
- A code search engine. `git grep` is the search engine, and the engine makes its patterns better.
- A wiki or a web interface. The MCP tools render the knowledge, and clients present it.
- Embeddings in the first version (§14).
- Proving absence. The engine says what it searched and what it has read (§6.2); it never stores that something does not exist, or that a list is complete.
- Deciding what is true. The engine records what the sources and people say, with evidence and attribution, and shows disagreement.

## 2. What carries over, and from where

### 2.1 From srtview: what carries over

| srtview | Why it matters here | askai's form |
|---|---|---|
| Content identity everywhere: a task's id is a hash of its inputs, and a cache travels to another machine | It deduplicates model work across users and time | Job ids (§8.2), git blob ids, the reply cache |
| Model output validated mechanically: citations inside the window, names on the cited lines, quotes copied from the source and never from the model | It is the guard against invented facts | §10 |
| Regular expressions as the currency: named topics, spellings knitted into one pattern (`rx::knit`) | Patterns are search keys that run at any revision | Terms, and their patterns for `git grep` (§3.2) |
| The projection as a function of the stored facts, independent of the order of completions (`DESIGN.md` I8), checked by a digest (`dirHash`) | Views are reproducible and testable | The view and its digest (§5); time decides nothing (§3.7) |
| Records as triples with a sentence and cited spans; byte-identical twins as one concept; judged pairs (same, related, contradicts, novel) | It is a workable shape for knowledge between named things | Claims, assertions and verdicts (§3.3, §3.4) |
| "A verdict nobody reads was not worth asking for": contradictions travel with the claims they contradict | Model work is spent only where something reads its result | Bundles (§6.2), nominations (§3.4) |
| A probe proposes a search, the corpus answers it, and the write sees only what the search found | A cheap job produces a mechanical query, and the costly reading is of real evidence | The connect job (§8.1) |
| Recipes: prompt, schema, model and settings in the identity of a cached reply | A changed prompt asks again, and an unchanged one never does | Recipes (§8.2) |
| Bounded fan-out (four judgment candidates, three focus partners), capped retries with feedback | The model's work per new fact is bounded | §3.4, §8.1, §8.2 |
| Retrieval weighted by how rare a word is in the corpus, with no word lists | It works in any language and needs no model | Ranking (§6.2) |
| Heat from user actions steers the background work | Work follows what people ask | Demand (§8.3) |
| "Waiting is a dependency"; workers report and never coordinate; one owner serializes scheduler and projection state (I7) | It keeps concurrency simple | The kb loop (§9) |

### 2.2 From srtview: what does not carry over

| srtview | Its problem | The rule here |
|---|---|---|
| Two ontologies, topic regexes (terms, dives, focuses) and semantic records, joined by a side channel (the lexicon) | Knowledge about one thing lives in two systems | One model: a claim's subject and object are terms (§3) |
| The projection rebuilt from scratch on every landing, on the UI thread: a 3.4 s refold per landing, measured [S] in `doc/core-design.md` | Work proportional to all state, for every event, on the serving thread | The view changes in small steps on the kb loop, copying only what each change touches, and is published by reference (§5) |
| Pokes, `landed()` counters swept per tick, harvest budgets of four per tick, a 1 ms timer that scans one video per tick, retire sets, the seams before and after a cut | Many small mechanisms decide when work happens | Each job ends in exactly one completion, a message to one owner (core R6), and work runs in bounded steps (core §1) |
| Mutable state shared between threads behind several mutexes (the executor's, the poke's barrier), with file I/O under a lock | Lock order and lifetime bugs, and stalls | One owner for each object (R1), immutable views (R5), no lock across I/O (R7) |
| Reply grammars of free text (`TERM:`, `SEEN:`, `REGEX:`, `MERGE:`, the last `SAME` or `DIFFERENT` of a reply) parsed by heuristics | Formats break silently | Each job answers with one call of one tool whose arguments a JSON schema fixes, and askai validates them (§8.2) |
| A catalog keyed by the recipes and the set of window ids: a new prompt, model or frame starts an empty view | Knowledge disappears with every prompt change until the corpus is asked again | A claim's identity does not include the recipe. Recipes key only the reply cache, and appear in provenance (§3.7) |
| State of the session rebuilt at every start by replaying cached replies | Startup cost and logic that replays | The knowledge base persists, and the view is built from its commit (§5) |
| Witnesses and cuts that keep asks from running over a half-read corpus | Machinery for a source that is read gradually | Sources are git commits, complete by construction |
| 64-bit truncated ids | Collisions in a large shared base | 128-bit ids (§3.7) |
| A JSON parser and encoder written by hand | Code to maintain | json-c and `jb.c`, as askai has them |
| Three votes for each pair of spellings, and asks about the whole term directory | Costly rituals for misheard speech | One judgment for each nominated pair. Code is not misheard, and its names vary systematically, so the ways one name is written are matched mechanically (§3.2), and only aliases, different names for one thing, need a judgment |
| Names folded into one entity whenever a model judged them one thing | One wrong judgment joins everything reachable through it | A model's judgment only widens search; people join entities (§3.4) |
| Patterns composed by name (`\{name:}`) | A pattern changes when another does, and references need checks for cycles | Patterns stand alone, each with its examples (§3.2); finding two names together is `connect`'s work (§8.1) |
| A summary pyramid over every video | Costly, and of little value for the questions in §1.1 | None by default (§14) |
| Thermal pacing and lanes inside the executor | Scheduling policy in two places | Home's slots, classes, fairness and breaker |
| Cut, witness, leaf, node, dive, probe, focus, heat, refinery, vault, scribe, loom, braid | Names that need explaining | Plain words (Appendix A) |

### 2.3 From the alternative design

An alternative design of the same engine, written independently and compared with this one, is the better of the two on what stored knowledge means and who may see it. This revision takes from it:

| The alternative design | Taken here as | Where |
|---|---|---|
| What a model writes in a conversation stays private; shared knowledge comes only from fixed extraction jobs that see source text alone | The boundary of a conversation: findings are private, turns cannot write, and an answer's citations only choose the lines that an `extract` job reads | §3.5, §7.4, §8 |
| Entities with a kind and a namespace; identical names do not imply identity; code identity not case-folded; no union-find over models' opinions; identity decisions that can be undone | Scopes for terms; names in code kept exact; aliases one verdict deep; entities joined only by people, split by them, and contested when they disagree | §3.2, §3.4 |
| Applicability (environment, build, deployment, valid time) and classes of evidence | Context labels in every assertion, found by askai; dates as the model's reading; a class for every span of evidence | §3.1, §3.3 |
| Lifecycle, support, review and freshness as independent dimensions, and no confidence score | Support, review and freshness, each derived and each shown | §3.9 |
| "Interpretation not revalidated" | Freshness speaks of the evidence, never of its meaning | §3.8 |
| Coverage with every result; negative claims only with complete coverage | Coverage with every result; no negative claims at all | §3.3, §6.2 |
| `core.fsync` hardening, and a compare-and-swap of the published ref | Every write synced, directories included, before its call is answered, at a measured cost; one `git update-ref` transaction that checks the tip of every branch it moves | §4.3 |
| A bounded dialect of regular expressions on PCRE2's DFA matcher, with positive and negative fixtures | Names matched without regular expressions; written patterns in a bounded dialect, with examples | §3.2 |
| A review tool apart from the tool that writes, not lent to models | `review` | §7.3 |
| A strong cheap baseline: scoped search and one answer call | The gate of the evaluation | §12 |

What this design does not take, and why:

- **Opaque ids, with names as attributes.** A term's identity is its scope and its name, so contributions of one name merge without an allocator, a lookup or a judgment, and a retried write is the same write. A rename is an alias, or a `replaces` claim. Scopes keep identical names in different repositories and namespaces apart, and names in code keep their exact form, which is most of what opaque ids solve; two things of one name within one scope remain open (§14). The names of systems are folded across case, because one service is written `PaymentService`, `payment-service` and `PAYMENT_SERVICE` in code, configuration and prose, and connecting those is the engine's work.
- **Snapshot handles on every call, receipts from a separate matching call, idempotency keys and revision preconditions.** The server derives all of these: calls read the latest view and name it, evidence is read from the lines that a contribution cites, and identity by content makes a repeated write a no-op. Three tools with small schemas leave a small model fewer ways to fail than six whose input schemas take about 22 KB together, some 5,500 tokens, half of it the schema of the tool that proposes.
- **Files laid out by id.** Measured in §4.1: at a million files, a commit of 256 claims costs 6 to 8 s and some 30 MB that way, against 30 to 160 ms and 0.1 to 0.4 MB with files laid out by where writes land.
- **A derived SQLite index, and threads for matching.** The view lives on the kb loop, bulk matching runs in `git grep`'s processes, and the core adds threads only for calls that can only block, so that runnable threads stay at N (core §1).
- **Stored negative and exhaustive claims.** They hold for one search at one revision, and keeping them true needs every change in their scope tracked. A bundle reports its coverage instead, at the current revisions.

## 3. The model

### 3.1 Sources, revisions and evidence

A **source** is a git repository that askai lists with `--git`, named as the `git` tool names it. Each source has a tracked ref, `HEAD` unless configured otherwise, whose commit is its current **revision**. Everything is a git repository: documents from other systems (wiki pages, tickets, runbooks, API catalogs, inventories of what is deployed) enter by being exported into a repository that askai lists, and what people state is stored in the knowledge base itself (§3.6).

The configuration (§7.4) gives each source a **namespace** (§3.2), the same one for every source unless configured otherwise, and rules that give its paths a **class** and **context labels**. A class says what a span's lines can show:

| Class | What its lines can show | Paths that have it by default |
|---|---|---|
| `code` | what the code can do, under the conditions it states | every path that no other rule matches |
| `configuration` | what a versioned setting selects, not that it was deployed | `*.yaml`, `*.yml`, `*.toml`, `*.ini`, `*.conf`, `*.properties`, `*.tf` |
| `document` | what a document says, which can disagree with the code | `*.md`, `*.rst`, `*.adoc`, `*.txt` |
| `observation` | what was observed, such as an exported inventory of deployments, as of its commit | none; a source or a path is configured as one |
| `testimony` | what a person stated (§3.6) | the knowledge base's testimony |

**Context labels** are the configured names of environments, builds and deployments, such as `prod`, `staging` or `eu-batch`, each with the other names that the sources call it by (`production` for `prod`). A path rule gives the labels that all of its lines are about (`config/prod/**` gives `prod`), and a claim also takes every label that its cited lines name. Labels are found, never chosen: askai attaches them when it writes a claim, by the configuration of that time (§3.3).

**Evidence** is a range of lines of a file at a revision: the source, the commit, the path, the first and last line, the blob, the class of its path, and the **quote**, which askai copies from the blob's bytes. Output of the `git` tool is never evidence: askai reads the cited lines itself. A claim has at most 8 spans of evidence, each at most 60 lines and 4 KiB. A span of testimony names the testimony and its lines instead of a source and a path. The quote is stored, so evidence outlives a source whose history is rewritten.

### 3.2 Terms

A **term** is a named thing within a scope:

| Field | Meaning |
|---|---|
| `name` | The name as written, such as `PaymentService`: as a person gave it, or else the first of its forms in byte order |
| `scope` | Where the name means one thing: its source, its namespace, or global |
| `kind` | `symbol` or `file`, names in code; `organization`; or one of service, job, store, channel, host, environment, team, document, process and concept |
| `forms` | The ways the name is written: `PaymentService`, `payment_service`, `PAYMENT-SERVICE` |
| `pattern` | Optional: a pattern that a person wrote, with its examples |
| `by` | Who added what: jobs, rules and identities, with times (§3.7) |

**Scope.** A name means one thing within its scope. Names in code, of the kinds `symbol` (a function, type, constant, variable, field or package) and `file`, are scoped to their source, so `Config` in twenty repositories is twenty terms. Organizations are global, since Nordea is the same bank to every system. Every other kind is scoped to its source's namespace: a service that one repository implements and another deploys is one term while the two share a namespace, and two business units' queues named `settlement` stay two terms once their sources are configured into two namespaces. The kind decides the scope, so a job is told the three groups of kinds (§8.1). A name that has terms in several scopes shows all of them, and a search or a walk that reaches a name in code also lists the terms of the same normalized name in other sources, marked as matches of the name alone: `SettlementRecord` used in five repositories is five terms, found together, and the client's model judges which are one thing.

**Identity.** A term's identity is its scope and its name. A name in code is kept exactly as written, since `Settle` and `settle` are two functions in Go, `FOO` and `foo` two names in C, and `config/prod-sftp.yaml` and `config/prod/sftp.yaml` two files. Any other name is normalized: its letters and digits, without separators, folded to lowercase, so that `PaymentService`, `payment-service`, `PAYMENT_SERVICE` and "payment service" name one service, whose forms are kept for display. Case is folded in ASCII; bytes outside ASCII count as letters, without case. Identity never joins two different names: `paysvc` and "payment service", or NDEA and Nordea, are two terms until a verdict makes them aliases (§3.4).

**Matching.** A text is split into words at characters other than letters and digits, at changes of case (`PaymentServiceClient` gives payment, service and client; `HTTPServer` gives http and server) and between letters and digits. A term occurs where a run of consecutive words, with at most one separator (`_ - . / :` or a space, or the `::` of qualified names) between two of them, spells its normalized name. So "payment service" occurs in `PaymentServiceClient`, `payment_service.yaml` and "the payment service", but not in `repaymentService`. In prose, that is in questions, statements, testimony and the lines of documents, the last word may also take a short suffix, at most four letters and at most half as many as the name has: plurals, and the inflections of languages such as Finnish ("Nordealle"), as srtview's term matcher allowed "Jiran" for Jira. Names in code are matched by their normalized form too, so a question about the payment service also finds the class `PaymentService` and the module `payment_service`, each a term of its own. askai matches names itself, in one pass over a text's words that walks a trie of the normalized names, so matching costs at most the text's length times the longest name's, whatever the names are, and needs no regular expression.

**The pattern of a term.** Over the sources, the kb loop finds a term's **occurrences** with `git grep -E -i` under `LC_ALL=C`, with a pattern made from its normalized name: its letters and digits in order, with an optional separator between any two of them, so that `nordea` gives `n(::|[-_./: ])?o(::|[-_./: ])?r…`. The pattern finds every line where the term occurs, whatever words the line splits the name into, and some lines besides, where the name is part of a longer word; askai's own matching then keeps exactly the lines where the term occurs. A prototype of both, run over every line of this repository and srtview's for 14 names, found no line that the pattern missed. A search for several names at once, such as an entity's terms and their aliases, knits their patterns into one (`rx::knit`). The pattern is the term's search key: it runs again at any revision, and finds the evidence again after code moves. The forms are kept for people to read.

**Written patterns.** A person may give a term a pattern for what its name does not cover: `ERR_SETTLE_[0-9]+` for settlement errors, or `settle(ment)?_retry`. A written pattern is in a bounded dialect, the part that POSIX extended expressions and PCRE2 read alike when they select lines: literal characters, `\` before a character that is otherwise special, `.`, bracket expressions of single ASCII characters and of ranges within `0-9`, `A-Z` or `a-z`, without backslashes, groups, alternation, `^` and `$` for the ends of a line, and after any of these exactly one repetition, `?`, `*`, `+`, `{m}`, `{m,}` or `{m,n}`, with counts up to 16. Nothing else is accepted, so there are no backreferences, no lookaround and no classes such as `\w`. Outside these rules the two engines part: under `-i` glibc reads `[A-z]` as letters only and refuses `[Z-a]`, and PCRE2 reads `a+?` as lazy and `a*+` as possessive. Within them, every pattern tested selected the same lines in `git grep -E -i` under `LC_ALL=C` (git 2.43) and in PCRE2's DFA matcher (10.42), over 20,000 test lines. A written pattern is at most 1 KiB, matches no empty string, and matches case-insensitively in ASCII, as names do. It comes with examples: cited lines that it must match, at least one, and lines that it must not, which are stored with it and checked whenever it changes, by PCRE2 and by glibc's `regcomp`, which `git grep -E` uses; a pattern on whose examples the two disagree is refused. A pattern does not refer to other patterns. A term with a written pattern occurs, too, where its pattern matches.

**Bounded matching.** Over the sources a written pattern runs with `git grep -E`, in git's processes. In askai's process it runs over the lines of questions and quotes, and over its examples, with PCRE2's DFA matcher, which never backtracks. Unanchored, that matcher starts again at every position of a line, so its cost grows with the square of the line's length: a line of 8 KiB took 1.3 to 1.5 s against `(a|b)*[0-9]` (PCRE2 10.42). askai anchors the pattern behind a leading `.*` instead, so that one pass over a line tries every start at once (0.6 ms for the same line), and gives the matcher room for 64 live states, so that a pass costs at most the line's length times 64. A pattern that needs more states fails that match, which is logged as no match, and a pattern that fails on its own examples is refused. An alternation of 40 words fitted, and one of 60 did not: a longer list of names belongs in terms.

**Limits.** A normalized name has at most 128 letters and digits. A term has at most 32 forms, the first 32 in byte order when merged contributions offer more. A name may be as common as the estate makes it: the company's name, or a core service's, occurs everywhere. Ranking weighs such names down by how common they are (§6.2), and listings of occurrences are capped.

### 3.3 Claims

A **claim** is one assertion with its evidence:

| Field | Meaning |
|---|---|
| `kind` | `fact`, `rule` (a constraint or a policy), `step` (a step of a procedure) or `definition` |
| `subject` | A term |
| `relation` | One of the fixed set below |
| `object` | A term, or a `value`: a literal such as "02:00 every night" |
| `context` | The context labels that the claim is about, found when it is written: those that the rules of its evidence's paths give, and those that its cited lines name (§3.1). Empty means unstated, never everywhere |
| `valid` | The dates between which the claim holds, when its evidence states them: the model's reading, shown with the quote, and no part of the claim's identity |
| `detail` | The source's own phrase for the relation, at most 128 bytes: "uploads settlement files over SFTP to" |
| `statement` | One sentence that stands alone, at most 1 KiB |
| `evidence` | Spans (§3.1) |
| `by` | Who asserted it: jobs, rules and identities, with times (§3.7) |

**Relations.** A fixed set makes the questions about relations mechanical walks of the graph: "what sends to X?", "what reads Y?", "what breaks if Z changes?". The view knows each relation's inverse, so queries go either way.

| Relation | A … B | Inverse |
|---|---|---|
| `is_a` | A is a kind of B | kinds |
| `part_of` | A is a part of B | parts |
| `calls` | A calls B's interface: an API, an RPC or a function | called by |
| `sends_to` | A sends data, messages or files to B | receives from |
| `reads` | A reads store B | read by |
| `writes` | A writes store B | written by |
| `depends_on` | A needs B to build or run, beyond the above | needed by |
| `runs_on` | A runs on B: is deployed, scheduled or configured to run there, as the class of the evidence says | runs |
| `configured_by` | A's behavior is set by B: a file, a setting | configures |
| `owned_by` | A is owned, maintained or operated by B | owns |
| `documented_in` | A is described in B: a document, a ticket, a decision | documents |
| `replaces` | A replaces B | replaced by |
| `has` | A has the property B, usually a value | — |
| `relates_to` | Anything else; `detail` says how | relates to |

The set belongs to the format (§4.2). A new relation needs a new version of the format, and old files stay valid under it.

**No negatives.** No relation denies, and no claim says that a list is complete. That nothing calls a function, or that these are all the writers of a table, holds only for one search over one set of files at one revision, and stops holding when a file is added. A bundle reports what it searched instead (§6.2). A person who knows a negative can state it as testimony, attributed to them, or as a rule: "card numbers must not be sent to the partner".

**Grounding.** A claim cites lines that name its subject and, when its object is a term, its object: the terms must occur in the quoted lines, by name or by written pattern (§3.2), which may take two spans, in two files if need be, when the names stand apart. A name also counts as named when a directory or file name in the path of the evidence spells it, as `cmd/settlement-uploader/upload.go` names settlement-uploader, since the path is part of the evidence that a reader sees. A value and dates are the model's reading of the lines and are not checked, which is why every reader is shown the quote. Things that the source does not name, such as "the retry mechanism", can live in statements but cannot be subjects. So every claim is anchored to names that matching finds again, and the person who reads its evidence sees both sides of the claim in it.

### 3.4 Assertions, verdicts, aliases, entities and merged claims

An **assertion** is a claim without its evidence: its kind, relation, subject, object and context. A claim is one observation of an assertion. The same assertion seen in two places, or again at a later revision, is two claims of one assertion. The same relation in another context is another assertion, so a staging configuration that sends straight to a partner stays apart from production's route.

A **verdict** judges two assertions or two terms, `same`, `related`, `contradicts` or `unrelated`, or one assertion, `confirm` or `dispute`, with a reason. People give verdicts with `review` (§7.3), and models in `judge` jobs (§8.1). Verdicts judge assertions, not claims, so a verdict holds for every observation of what it judges, the later ones included. A verdict records what its judge was shown, the commit of the knowledge base that a person's call names or the job that a model answered, so a reader can see whether the evidence has changed since. A verdict is an event and never changes: a person changes their mind with a new verdict that names the one it replaces, and the view reads the verdicts that nothing replaces; should two of a person's verdicts replace the same one, the one with the smaller id stands. So what the view makes of verdicts depends on which verdicts exist, never on the order in which they arrived.

**People decide, models advise.** The view resolves the verdicts on an object from their set alone. People's current verdicts decide: those that agree give their value, those that disagree make the object **contested**, and a contested `same` joins nothing. A model's verdict joins, merges and withdraws nothing: its `same` makes two terms aliases, its `contradicts` makes two assertions travel together, and its `related` and `unrelated` are shown with what they judge. A person's verdict on the same pair overrides it. A pair that has a model's verdict is not judged again by a model.

**Aliases.** Two terms are **aliases** when a verdict, a person's or a model's, judges them the same, and no person's verdict on them disagrees. Aliases are one verdict deep: a search that matches a term also matches the terms that a verdict judged the same as it, but not their aliases in turn, and shows each alias's claims under its own name, with who judged them the same. Aliases widen what is found and join nothing.

**Entities.** Terms that people's `same` verdicts join form one **entity**, and no model's verdict joins anything, so one wrong judgment of a model cannot spread through a chain of joins. An entity in which a person's `unrelated` separates two members, as when A is joined to B and B to C but A is judged unrelated to C, is contested: it joins nothing until people resolve it, and it is shown with the verdicts that conflict. A join is undone as it was made, by a person's new verdict. An entity's title is the name of its term with the most claims, ties going to the first name in byte order.

**Merged claims.** Assertions with the same kind, relation and context, the same subject entity and the same object (an entity, or a folded value) are one merged claim, mechanically: srtview's rule that one claim seen in two places is identity, not a judgment. A person's `same` verdict merges other assertions; a model's shows them side by side. A merged claim lists the evidence of all its claims, and its statement is the longest that a person wrote for it, or else the longest of all, ties going to the first in byte order. An assertion that people dispute and no person confirms is **withdrawn**: it is left out of bundles (§6.2), and `show` and `related` still list it, with its disputes. A claim that contradicts another travels with it in every bundle.

**Nothing is counted.** More claims of one assertion are more places that show it, not more confirmation, and two models that agree are not two witnesses. No rule of the view counts claims, models or verdicts.

**Nominations.** Pairs are never judged wholesale. A new assertion is judged against at most three assertions that share its subject entity and its context and overlap it most in words. A new term is judged against at most two terms whose names share a word, whose initials spell it (PSP and "payment service provider"), or whose letters it takes in order (`paysvc` and "payment service"). Nominations wait for demand (§8.3): a pair that nobody asks about is not judged.

### 3.5 Findings, and the boundary of a conversation

A **finding** is an answer that `ask_ai` gave and askai kept: the question and its system text, the digest of the bundle that the upstream was given (§6.3), the answer, the citations that askai could validate (claims, and spans of sources), the terms that the question matched, the commits of the knowledge base and of the sources it was answered at, the recipe, and the time. Before it is stored, the secret rules (§10) blank what they match in it. A finding is its asker's. It is kept in the asker's own repository (§4.1), which only that principal's calls read, and it is offered to them as an earlier answer, with the freshness of what it cites. An anonymous asker's answers are not kept, since anonymous clients share one principal, but their citations queue a job as any answer's do.

**The boundary.** A conversation can hold confidential plans, pasted secrets, notes, and premises that are false. What a model writes in it, its answer and anything it writes on the way, is a product of all of that, and a citation does not change this: a claim that a model writes while it answers can carry the asker's premise into a sentence and attach it to real lines that name the same things. So nothing that the upstream writes in a conversation becomes shared knowledge. Findings are private, the turns of an ask call can read the knowledge base but not write to it, and shared knowledge comes from three places only: jobs that are shown source text and shared knowledge, never a question, an answer, a note or a draft (§8); rules that match the sources (§8.5); and people, who answer for what they contribute (§7.2). The research is not lost: an answer's validated citations of lines queue one `extract` job over the windows that hold them (§7.4), a job whose id is the same whichever question queued it. What the knowledge base shows of a question is which jobs it caused: that some lines were read, and some terms described or pairs judged or connected (§8).

### 3.6 Testimony

**Testimony** is what a person states through their client with `contribute`, at most 8 KiB of text that the secret rules must pass. It is stored as a file in the knowledge base, attributed to the identity, and claims can cite its lines as they cite a source's (§3.1), with the class `testimony`. Testimony is how knowledge that is in no repository gets in: contracts, schedules, contacts, the reasons for decisions. Only clients testify: the upstream has no testimony to give, and cannot write (§3.5). A client's testimony is attributed to the client's identity, which answers for it whether a person typed it or the person's assistant did.

### 3.7 Identity

| Object | Its identity hashes | Not hashed |
|---|---|---|
| Term | its scope, and its name: exact for a name in code, normalized for any other | the name as written, where it is normalized; the kind within its group of kinds; forms, pattern, provenance |
| Assertion | kind, relation, subject, object (a term, or a folded value), context labels | — |
| Claim | its assertion, and its evidence (source, path, blob, lines) | statement, detail, dates, quotes, classes, provenance |
| Verdict | the objects it judges, sorted; who gave it (an identity's name, or a model's recipe); its value and reason; what its judge was shown; the verdict it replaces | time |
| Finding | the asker's principal, the question, system text, answer, citations and time | — |
| Testimony | identity, text | time |
| Job | kind, recipe, every byte the model is shown | — |

An id is the first 128 bits of SHA-256, over a tag naming the type and the canonical fields, written as 32 hexadecimal digits. The tools take any unique prefix of at least 8 digits.

**Time decides nothing.** Contributions carry the time they were made, for people to read. No rule of the view, and no rule that merges a file, chooses by time, so the knowledge base and its view depend only on what was contributed, never on the order in which it arrived, but for the recorded times themselves (srtview's I8). A finding is an event, so its time is part of its identity. A testimony is identified by who gave it and its text, so the same statement given twice, or retried, is one. A person's later verdict replaces their earlier one because it names it. A claim stays true of the commit that it cites; the same assertion at a new revision is a new claim of the same assertion.

### 3.8 Freshness

At a source's current revision, each span of evidence is:

- **unchanged**: its blob is unchanged, or its quote is still at its lines;
- **moved**: its quote is found verbatim at other lines of the same file, or in the file that `git diff-tree` reports it was renamed to;
- **changed**: neither, but the claim's terms still occur in the file;
- **gone**: none of these.

Freshness speaks of the evidence, never of its meaning. Unchanged lines can mean something else when a constant, a macro, a header, a dependency, an override in configuration or a deployment changes elsewhere, and the engine does not follow what lines depend on (§14). So a claim whose evidence is unchanged is shown as "evidence unchanged, interpretation not rechecked", never as current or valid.

A quote names its claim's terms (§3.3), so a quote found again is the same text, not a stray `}`. A claim's freshness is that of its best span. A moved claim keeps its file: the view records where its quote is now, and nothing is asked or written. When a tracked ref moves, the kb loop asks `git diff-tree` for the paths that changed, and checks only the spans in those paths, each with a read of the new blob and a search of its bytes. No model is asked. Freshness is not stored: it is a function of the knowledge base, the sources' revisions and the outcomes of rechecks, and cheap to compute again. A changed claim is a candidate for a recheck job (§8.1) when there is demand for it, and a recheck that finds no support makes it count as gone. A gone claim is history: still true of its commit, and offered last.

### 3.9 Support, review and freshness

Every claim that a tool shows carries three dimensions. Each is derived from what is stored, each says one thing, and none is a score:

| Dimension | Values | Derived from |
|---|---|---|
| Support | `observed`: a rule found it (§8.5); `inferred`: a job's model read it from the source; `stated`: a person contributed it | the claim's provenance, so a claim can have more than one |
| Review | unreviewed, confirmed, contested or withdrawn | people's current verdicts on its assertion (§3.4) |
| Freshness | unchanged, moved, changed or gone | the sources' current revisions (§3.8) |

They stay apart: a person's confirmation does not make an inferred claim observed, a claim that a person stated is not thereby checked, and unchanged evidence is not a meaning checked again. They are not folded into one confidence, because a number would hide which of them a reader should doubt. Ranking uses them (§6.2) and shows them, with the class of each span of evidence and the context.

## 4. The knowledge base in git

### 4.1 Layout

```text
DIR                            --kb DIR, made if it does not exist
  knowledge.git                bare, shared
    refs/heads/knowledge
      FORMAT                   the version of the format, and its relations
      terms/namespace/payments/se/settlementuploader.json
      terms/source/payments/pa/paymentservice-3f9a01c2.json
      terms/global/no/nordea.json
                               one file per term, under its scope and the
                               first two characters of its normalized name
      claims/sources/payments/src/settle/upload.go/5d1e...07.json
                               one file per claim, under the file that its
                               first span cites
      claims/testimony/carol/7e21...5a/c0ff...ee.json
                               the claims that cite one testimony
      verdicts/a0/a0c4...9b/5b7e...19.json
                               one file per verdict, under what it judges
      testimony/carol/7e21...5a.md
                               one file per statement, as given
    refs/heads/replies
      extract/2026-10-14/4c1f...08.json
                               each job's reply and outcome
      recipes/91be...2d.txt    each recipe's prompt, schema and settings
  findings/<principal>.git     bare, one for each asker
    refs/heads/findings
      2026-10-14/0c4d...11.json
                               the asker's findings
```

Findings live apart, a repository for each principal, as the notes keep one for each thread: cloning the shared repository copies nobody's questions, no principal's name is a ref in it, and deleting a person's findings is deleting their repository.

Paths follow where writes land. The claims of one `extract` job cite one file, so its commit touches that file's directory and the few above it; findings and replies land in the day's directory, and testimony in its giver's. A commit thus rewrites a few small trees. A fan-out by id would scatter every batch over most directories and rewrite most of the tree, at a cost that grows with the knowledge base. Measured with `git fast-import` from the branch's tip (git 2.43, 4 CPUs; claim files of about 700 bytes, in a synthetic estate of 20 sources with 1,000 directories and 10,000 files each; scratch work, not in the repository):

| Commit | 100,000 files, by id | 100,000 files, this layout | 1,000,000 files, by id | 1,000,000 files, this layout |
|---|---|---|---|---|
| 1 new file | 13–21 ms, +24–26 KB | 10–12 ms, +2.2–2.3 KB | 32–37 ms, +178–188 KB | 10–13 ms, +2.5–2.8 KB |
| 256 claims of one source file | 256–306 ms, +3.0–3.1 MB | 33–44 ms, +131 KB | 6.5–7.4 s, +28–29 MB | 29–38 ms, +131 KB |
| 256 claims of 256 source files | 277–323 ms, +2.9–3.2 MB | 103–117 ms, +310 KB | 5.8–7.7 s, +27–30 MB | 100–156 ms, +404 KB |

Under this layout a commit costs about what its own files do, whatever the size of the knowledge base. The layout also reads well: what the knowledge base says about a file is found under that file's path. A term's file is named by its normalized name, which has only letters and digits, under its scope and the name's first two characters, and a name in code also by the first 8 digits of its id, since several names in code can share one normalized name. These commits were not synced to disk; §4.3 measures what syncing costs.

### 4.2 Files

Each file is canonical JSON with its members in a fixed order, one member per line, so that a diff reads as a change of fields. A file's content is a function of the set of contributions to its object, by rules that read only the values: provenance and forms are sets, kept sorted and capped (one entry of provenance for each contributor, an identity's name, a rule's id or a job's id, at most 16, people's first by name and then the others' by id; 32 forms in byte order), and a single field takes a person's value over a model's, then the longest statement or the first other value in byte order. The writer rewrites a file when a contribution adds to it, and the order in which contributions arrived never shows in the tree (§3.7).

A claim file:

```json
{
"format": 1,
"type": "claim",
"kind": "fact",
"subject": "3f9a01c24e5b8d77a6c1e0f2b9d4c3e1",
"relation": "sends_to",
"object": "91be07aa3c5d4f6e8a9b0c1d2e3f4a5b",
"context": [],
"detail": "uploads the daily settlement file over SFTP to",
"statement": "The settlement-uploader job uploads the daily settlement file to Nordea's SFTP server.",
"evidence": [
{"source": "payments", "commit": "1a2b...", "path": "src/settle/upload.go", "blob": "77c0...", "lines": [38, 47], "class": "code", "quote": "..."}
],
"by": [
{"identity": "carol", "at": "2026-10-14T13:05:40Z"},
{"job": "4c1f...", "recipe": "91be...", "at": "2026-10-14T13:02:11Z"}
]
}
```

### 4.3 One writer

The kb loop is the only writer. It gathers accepted contributions for at most 50 ms, or until it has 256 objects, and writes them with one run of `git fast-import` for each repository that the batch touches, from the tips that it read: on `knowledge`, one commit for each identity that contributed, with that identity as its author, and one for the contributions of jobs and rules, with askai as its author; on `replies`, the replies of the jobs whose results the batch holds; and in each asker's repository, their findings. Each commit's message lists the ids of what it added or confirmed, also when a capped file did not change, so `git log --grep` finds every contribution. No message in the shared repository names a question or an asker.

**Durability.** gitstore runs every git that writes with `core.fsync=objects,reference`, `core.fsyncMethod=fsync` and `fastimport.unpackLimit=1`. By default git syncs the pack that `fast-import` writes and its index, but not the ref, nor the loose objects that it unpacks an import of fewer than 100 objects into, so a crash could lose a write whose call was answered, or keep a ref to objects that never reached the disk. With the first two settings, git also syncs loose objects and refs; the third keeps every import a pack, so an import syncs three files, the pack, its index and the ref, instead of one for each object (git 2.43, traced with `strace`). Git syncs no directory, though: it links the pack into place and renames the ref's lock over the ref, and until their directories are synced, a power loss can bring the old tip back. So before the calls answer, a blocker syncs the `objects/pack` and `refs/heads` directories of each repository that the batch wrote. Measured as in §4.1, in this layout, on this machine's virtual disk (ext4), five runs each; a cell gives the range at 100,000 files, then at 1,000,000:

| Commit | git's default, the ref not synced | Synced, small imports unpacked | Synced, every import a pack |
|---|---|---|---|
| 1 new file | 10–13 ms; 11–17 ms | 14–22 ms; 16–19 ms | 7.5–10 ms; 8–12 ms |
| 16 claims of one source file | 14–23 ms; 14–17 ms | 23–32 ms; 26–28 ms | 9–18 ms; 9–11 ms |
| 16 claims of 16 source files | 26–41 ms; 30–58 ms | 54–94 ms; 64–84 ms | 17–32 ms; 19–21 ms |

Imports of 100 objects or more are packs in any case, and syncing them cost nothing measurable (29–41 ms for 256 claims of one source file). So the import is durable and cheaper than git's default makes it, at the price of more small packs, which the geometric repacking of §4.5 rolls up. The transaction that moves the tips (below) and the syncs of directories come on top, 10–18 ms and under 1 ms more on the same machine, so a durable write of a small batch takes some 20 to 50 ms. E0 measures the whole write again on the deployment's filesystem.

**Compare and swap.** `fast-import` checks only that a branch's new tip contains the tip that the branch has when the import ends, and moves each branch in a transaction of its own (`update_branch()` and `dump_branches()` in git's `builtin/fast-import.c`). That is not enough: a batch built on a tip that a curator has since rewound exits 0 and brings back what the curator removed (git 2.43, tested). So `fast-import` writes to staging refs, and one `git update-ref --stdin` transaction then moves every branch of the repository from the tip that the kb loop read to its new one, or, if any of those tips has moved, moves none (tested). The kb loop then takes the edit in (§4.5) and writes the batch again. If the write fails otherwise (a full disk, a lock, an error of git's), the calls waiting on the batch answer with the failure, so no caller is told that a write succeeded when it did not; the jobs' results stay in memory and are written again after a backoff. While it runs, the kb loop holds an advisory lock (`flock`) on the directory, which the kernel releases if askai dies, so that a second askai cannot write the same knowledge base. At start it removes the lock files that are older than 60 s, as the notes do, since no git of askai's runs that long; a younger one may be a curator's push in progress.

### 4.4 What is not stored

Occurrences, freshness, coverage, indexes, scores and the mentions that rules find (§8.5) are not stored: they can all be computed again from the knowledge base and the sources, and storing them would make the repository churn with every commit of every source. Demand (§8.3) is not stored either. It is advisory, it is lost when askai stops, and it builds up again with use.

### 4.5 Operations

- **Packing.** Every import is a small pack (§4.3). After every 256 commits the kb loop runs `git repack --geometric=2 -d`, which rolls the small packs into fewer and costs little when it runs often; no batch is written while it runs, so no write races it. A whole-repository `git gc` runs only when an operator asks, and the kb loop holds its writes meanwhile.
- **Retention.** The `replies` branch is a cache. Pruning it by age drops the texts of old replies but keeps each job's id and outcome, so pruning never makes the engine ask again. A person's findings are deleted with their repository.
- **Replication.** The shared repository can be cloned and fetched, and holds no finding. A view builds from any commit, so a read-only askai can serve `knowledge` from a mirror.
- **External edits.** A curator may edit the `knowledge` branch directly, for example to fix a term's kind or the way its name is written, but not its scope or normalized name, which are its identity: a real rename is a new term, which a person's `same` joins to the old one or a `replaces` claim relates to it. When the kb loop finds a tip that it did not write, it validates the changed files as it validates contributions (§10), applies the valid ones to the view, and logs the invalid ones, which every view built later skips in the same way. A person withdraws a claim by disputing its assertion (§3.4). Deleting the file would not do, because a cached reply would add it again.
- **Audit.** `knowledge show` gives an object's history, the commits that added or confirmed it and its verdicts, and with `at` the object as an earlier commit held it. `git log` of its file, and `git log --grep` with its id, give the same from git.

## 5. The view

The **view** is an immutable, in-memory index of one commit of the `knowledge` branch and of the askers' findings, with a layer of freshness for the sources' current revisions. It holds:

- terms by id, and a trie of their normalized names, for matching texts (§3.2);
- the written patterns, compiled for PCRE2's DFA matcher;
- entities and merged claims, as maps from each member to its group, and each term's aliases;
- claims and assertions by id, and postings from each entity to its assertions, by relation, direction and context;
- verdicts by object, and findings by term, each finding with its principal, whose calls alone read it;
- word statistics (how many statements and names each word occurs in) for ranking, which count no question, since questions are private;
- the freshness of each claim at the current revisions.

**Persistent maps.** The maps are hash tries, as Clojure's maps are: a change copies the nodes on its path, a handful of them (the logarithm, base 32, of the map's size), and the new view shares every other node with the old one. A batch of 256 objects copies some hundreds of small nodes, not the view. Postings grow by appending. Groups grow by relabeling the smaller group of a union, and a verdict that splits a group (a person who replaces their `same`, or another person who contests it) recomputes that group alone.

**Changes.** The kb loop applies a commit's objects to a new view in bounded steps on its own thread, and publishes the view by posting a reference to every client loop, at most one view every 100 ms. A loop takes the new view for the calls that begin from then on and lets go of the old one, as it does with a reloaded policy (core §6, R5). A call holds a view only while its steps use it: a `knowledge` call for its milliseconds, and an `ask_ai` call while it builds its bundle and again while it checks its citations, never across its turns. An old view thus lives for milliseconds after its successor, and keeps alive only the nodes that its successor replaced.

**A caller's own writes.** A `contribute` or `review` call answers only after the view that holds its write has been posted to the call's loop, and a loop handles each producer's messages in order (core §2), so the next call on that loop sees the write.

**Which view answered.** Every result names the commit of the view that answered it, so a reader can tell when two answers came from different states of the knowledge base. `show` can also read an object as an earlier commit of `knowledge` held it, from git (§7.1); searches always use the latest view.

**Determinism.** A view's digest is a hash over its canonical dump, recorded times left out, and depends only on the trees of the branches it reads and the version of the format. A test commits the same contributions in shuffled orders, with their recorded times shuffled too, and compares the digests. This is srtview's `dirHash`, made a gate.

**Size.** A claim takes about 1 KiB in the view, so 100,000 claims take about 100 MB. Beyond that, quotes and statements can stay in git and be read when shown, or a saved view can speed up starts, but never as the authority (§14).

**Start.** The kb loop builds the first view from the tips a subtree at a time: `git ls-tree` of a subtree, then `git cat-file --batch` of its files, each a child whose output is capped (16 MiB) and parsed in bounded steps, so memory stays bounded whatever the size of the knowledge base. The `replies` branch is indexed from its file names, which carry each job's id, day and outcome, and from the windows that its replies record having read (§6.2). Until the first view is published, `knowledge` answers that the knowledge base is loading.

## 6. Retrieval and the semantic cache

### 6.1 From words to terms

The text of a query is split into words as names are (§3.2), and one pass over them with the trie of normalized names finds every term that the text names, in every scope, the last word of a name allowing a short suffix. Written patterns run over the text with the DFA matcher. Matched terms expand to their aliases and entities. An identifier in the question that no term matches (camel case, snake case, a path) is looked up in the sources by the kb loop as a literal: a hit is offered as a candidate term, which a client can add with `contribute`.

### 6.2 Bundles

A **bundle** is what a query returns:

- **Terms** matched, with their scopes, kinds, aliases, entities, forms and definitions. A name that has terms in several scopes shows each of them, so that a reader can tell one repository's `Config` from another's.
- **Claims**, ranked: claims that link two of the matched terms first, since they answer relationship questions; then by the rarity of their shared terms and the overlap of their statements with the query, both weighted by how rare each word is (srtview's `vocab`); claims with unchanged evidence before changed ones, and gone ones last; confirmed claims up, contested ones marked, and withdrawn ones left out. A claim that contradicts a ranked claim comes with it. Words are compared by their first six characters, so "settlements" meets "settlement" and "Nordealle" meets "Nordea". srtview compared whole words; the cut needs no word list either, and it tolerates plurals and the inflections of languages such as Finnish. Each claim shows its support, review and freshness (§3.9), its context, and the class of each span of its evidence, so that "configured to run on prod-batch" is not read as "runs on prod-batch".
- **Findings**: the asker's own, whose terms and questions overlap the query, with the freshness of what they cite.
- **Occurrences**: lines where matched terms occur that no claim explains. The kb loop looks them up with `git grep` at the current revisions and keeps them by term and revision. A search waits for them at most 2 s, then answers with what it has and says what is still being looked up. They are raw material that the client's model can read itself.
- **Coverage**: what the bundle rests on. The commit of the view and the revision of each source; for each matched term, the number of files at the current revisions where it occurs, from the same `git grep` as its occurrences and given as "at least" while that search is capped or still running, and how many of them the knowledge base has read; and what is never read: paths excluded by rule and files beyond the limits of §8.4. A file counts as read when a job of any recipe has read every window of its current blob whole and replied validly. A reply records the windows that its job read, by a hash of each one's path and text, and the kb loop keeps, for each blob that it has cut into windows, how many of them have been read, updating the count as replies land; a blob that it has never cut counts as unread. So "the knowledge base knows three writers of `ledger_entries`, from 5 of the 14 files where it occurs" is not read as "these are its writers".
- **Gaps**: matched terms that have no claims, and pairs of matched terms that no claim connects.

A bundle never says that something does not exist: when no claim of a relation is known, it says that none is known, and its coverage says how much was looked at. Output is text, paged with `offset` as the `git` and `notes` tools are, and in a turn within a window of an eighth of the context.

### 6.3 Three levels of reuse

1. **The same question over the same knowledge.** An `ask_ai` call whose normalized prompt, system text, recipe and bundle digest, a hash of the ids of the claims in its bundle, equal those of one of the asker's findings is answered with that finding at once, marked as such, if what the finding rests on still holds: before freshness exists (E5), if the sources are still at the revisions that the finding recorded; after, if the evidence of its citations is unchanged. A new answer could be worded differently, but it would rest on the same knowledge; this is srtview's identity of an answer. A call that gives `reuse: false` skips this level, and so does a call that goes on with a notes thread. A finding is never reused for another asker (§3.5).
2. **A different question about the same things.** The bundle carries the claims and the asker's earlier findings, and the client's model or the upstream answers from them. The upstream often answers in one turn without calling a tool.
3. **Nothing known.** The upstream researches with `git` and answers, and an `extract` job reads what its answer cites (§7.4). The next question about it, from anyone, lands on level 2.

### 6.4 Who judges relevance

The server does not decide that two different questions are the same question. A wrong hit costs more than a miss, and judging equivalence with a model would spend the call that the cache was meant to save. The server matches mechanically and shows its candidates with their evidence, support, freshness and coverage. The client's model judges what applies: it knows the asker's context, which the server does not, and it runs on the asker's hardware, so its reasoning costs the upstream nothing. The cheap reasoning thus moves to where it is free, and the scarce upstream is kept for research.

## 7. Tools

Three tools, so that a role can grant reading, writing and judging separately; roles grant tools, not actions. None speaks of git: they name terms, claims and evidence, as the `notes` tool names notes and not commits. Git is how the knowledge is kept, not what a caller works with. Only `knowledge` is ever lent to the turns of an ask call, since writing or judging in a conversation would carry what was said in it into shared knowledge (§3.5).

### 7.1 `knowledge`

Read-only, lent to turns when the role's `upstream` member grants it (`TOOL_LENT`), paged (`TOOL_PAGED`). Every result names the commit of the view that answered it, and says what it covered.

| Action | Arguments | Result |
|---|---|---|
| `search` | `text`: a question, names or identifiers | a bundle (§6.2) |
| `show` | `id` or `name`, optional `at`: a commit that `knowledge` has had | the whole object: a term with its forms, pattern and examples, aliases, entity, claims by relation and top occurrences; a claim with its quotes, support, review, freshness and history; a finding of the asker's with its citations. With `at`, the object as that commit held it, read from git |
| `related` | `name`, optional `scope`, `relation`, `context`, and `depth` of at most 3 | the graph around a term, or around each term of the name when it has several and no `scope` is given, for impact questions: paths of claims, at most 16 edges a hop and 64 in all, each edge with its support, review, freshness and context, and the coverage of each term on the paths. Terms of the same name in other sources are listed as matches of the name. With `context`, claims of other contexts are left out and claims with none are marked. A path is a chain of claims, not proof that its ends affect each other |

### 7.2 `contribute`

Writes, for clients only, and never lent; a role grants it to the people who curate. A client's contributions are its identity's: stated (§3.9), attributed, and answered for by that identity, whether a person typed them or the person's assistant did.

| Action | Arguments | What askai does |
|---|---|---|
| `claim` | `subject`, `relation`, `object` or `value`, `statement`, optional `detail`, `kind` and `context`, `evidence`: `[{source, path, lines, rev}]`, or `{testimony, lines}` for testimony. Each name comes with its kind, and with its source or namespace when the evidence is testimony | posts the claim to the kb loop, which reads the cited lines at `rev` (by default the source's tracked ref, as it is when askai reads them), copies the quote, checks the claim (§10), writes it, and answers with the claim's id and the terms that it found or made, or with every reason for refusing it, worded so that the caller can correct the claim and try again |
| `term` | `name`, `kind`, `evidence`, optional `pattern` and `examples`: lines that the pattern must match and lines that it must not | checks that the name occurs in the cited lines and the pattern against its examples (§3.2), and adds the term or extends it |
| `testimony` | `text`, optional `about`: names | stores the text, attributed (§3.6), and answers with its id and the address that claims cite it by |

A write is durable and visible when its call answers: the reply waits for its batch's commit (§4.3) and for its view (§5), some tens to a few hundred milliseconds.

### 7.3 `review`

Judgments, for clients only, and never lent; a role grants it to the people who curate. A call records one verdict (§3.4): `ids`, one or two; `value`, `confirm` or `dispute` for one assertion, or `same`, `related`, `contradicts` or `unrelated` for two terms or two assertions; `reason`; `seen`, the commit of the knowledge base that the reviewer read, as results name it; and optionally `replaces`; without it, the new verdict replaces the reviewer's verdict on the same object that the view holds. Joining two terms into an entity, splitting them again and withdrawing an assertion are all verdicts, so each is undone by another. A verdict is attributed to the identity that gave it, as a contribution is.

### 7.4 In `ask_ai`

With `--kb`:

- Before the first turn, askai runs a search over the prompt. If one of the asker's findings matches on level 1 (§6.3), askai answers with it at once. Otherwise it adds the bundle to the prompt in brackets, as it adds notes, within an eighth of the context and `--kb-bundle` (16 KiB by default).
- With `--upstream-tools`, turns get `knowledge` as the role's `upstream` member grants it, and never `contribute` or `review`.
- After the answer, askai checks the citations in it (`[c:5d1e9b20]` for a claim, `[payments:src/settle/upload.go:38-47]` for lines): claims against the view, and lines by having the kb loop read them. It keeps the answer as a finding in the asker's own repository, with the valid citations. The bundle shows every claim with the citation that names it, so the upstream knows how to cite before it answers.
- The kb loop queues one `extract` job (§8.1) for the answer. Its excerpt is the windows that hold the lines that the answer validly cited, in the order of their sources and paths, or, when those do not fit in 24 KiB, the cited lines with 20 lines around each. So the job can relate names that the answer found in different files. It is shown the excerpt and nothing of the call; an excerpt of one window is that window's own job (§8.2), and an excerpt that has been read is not read again. The trace that the client receives says what was kept and what was queued.
- The files that the turns read with `git` add to demand (§8.3).

A configuration in which clients and the upstream read the knowledge base, curators write and judge as people, and three sources share the namespace `payments` (`kb`, `kb-contexts` and `kb-sources` are new members of the configuration file; `git`, `upstream-tools` and `roles` are askai's own, and the role's `upstream` member landed in 9bd5f42):

```json
{
  "git": ["/srv/git/payments.git", "/srv/git/deploy.git", "/srv/git/inventory.git"],
  "kb": "/var/lib/askai/kb",
  "upstream-tools": true,
  "kb-contexts": { "prod": ["production"], "staging": ["stage"] },
  "kb-sources": {
    "payments":  { "namespace": "payments",
                   "paths": [{ "glob": "config/prod/**", "context": ["prod"] }] },
    "deploy":    { "namespace": "payments",
                   "paths": [{ "glob": "k8s/prod/**", "context": ["prod"] }] },
    "inventory": { "namespace": "payments", "class": "observation" }
  },
  "roles": {
    "user":    { "tools": ["ask_ai", "knowledge"],
                 "upstream": ["git", "knowledge"] },
    "curator": { "tools": ["ask_ai", "git", "knowledge", "contribute", "review"],
                 "upstream": ["git", "knowledge"] }
  }
}
```

### 7.5 Notes stay separate

The `notes` tool stays as it is: private, per thread, unvalidated, the upstream's working memory. The knowledge base is shared, validated and attributed. Merging the two would either weaken the knowledge base's checks or burden the notebook with them, and notes, like everything else of a conversation, stay out of every job (§3.5). The two share plumbing instead. What the notes need of git, now split between `notes.c` (the fixed environment, argv, the input of `git fast-import`, the parsing of `git cat-file`'s output, the rules for names) and `mcp.c` (spawning git with deadlines, removing stale locks), becomes a module that both use (§13, E0), and the notes can take its settings that make writes durable (§4.3).

## 8. Jobs

Every job is shown source text and shared knowledge, and no text of any conversation: no question, answer, note or draft. A question can decide which jobs run: which lines are read, which terms described, which pairs judged or connected. That choice is all that the knowledge base keeps of it. A job's id hashes only what it is shown, so a job that a question queued is the same job as the reading of the same lines on demand, and what it yields is shared.

### 8.1 Kinds

| Kind | The model is shown | It answers | Queued when |
|---|---|---|---|
| `extract` | an excerpt of up to 24 KiB: blocks of numbered lines, each under its source and path, such as one window of a file (§8.4) or the windows that an answer cited (§7.4) | terms (name, kind and the forms written in the lines), and claims citing lines that it was shown | an answer cited its lines, there is demand on its file, or the file is a boundary file (§8.3) |
| `describe` | a term and an excerpt of up to 24 KiB of its occurrences: lines from several files, each block under its source and path | a definition and other claims citing those lines | a term that someone asked about has no definition |
| `judge` | two assertions with their quotes, or two terms with their claims or occurrences | a verdict and its reason | a nominated pair (§3.4) has demand |
| `connect` | two terms and up to 8 KiB of the occurrences of each | a pattern in the bounded dialect (§3.2) to search for code that involves both, with lines of its input that it must match, or none | a question relates two terms that no claim connects |
| `recheck` | a changed claim and the current text where its evidence was | whether the text still supports it, and the lines that do | a changed claim (§3.8) has demand |

Every job that names terms is told the three groups of kinds, since a name's kind decides its scope (§3.2); the groups belong to the format, and the recipe's prompt states them. No job is shown the configuration: the class of a claim's evidence and its context labels are attached after the reply, by the rules of its paths and the names in its lines (§3.1), so that a change of the configuration re-keys no job.

`connect` is srtview's probe, search and focus as one chain. Its pattern is checked against its examples and the other rules of §10, then run with `git grep -E` at the current revisions; up to 24 KiB of the lines it found, with their neighbors, become the excerpt of one `extract`, which the reply cache serves like any other. One `connect` thus costs at most two jobs, as srtview's probe and focus did. A `recheck` that finds support yields a new claim of the same assertion with new evidence. One that does not makes the old claim count as gone (§3.8), and the old claim stays true of its commit.

There is no answer kind, because `ask_ai` is how answers are made, and its answers stay with their askers. There are no summaries by default (§14).

### 8.2 One runner

- **Identity.** A job's id is a hash of its kind, its recipe and every byte that the model is shown, and of nothing else. srtview kept a video's title out of the identity because a renamed video is the same video. Here the path is shown and hashed, because a configuration file under `prod/` and the same file under `staging/` say different things.
- **Reply cache.** The `replies` branch records each job's reply and outcome, and the windows that it read whole. A job whose reply is cached is not asked again. If demand reaches a cached job whose claims the knowledge base lacks (a knowledge base rebuilt, or the cache carried to another deployment), its cached reply is validated again, without an upstream call. A job in flight with the same id takes the new demand, and nothing is submitted twice.
- **Submission.** Jobs go to home as a front's jobs do, in the background class, at most `--kb-jobs` at a time (2 by default; 0 runs none). Home refuses a job at once while its breaker is open, or when too many jobs are queued; the kb loop then submits nothing until the time that the refusal names has passed, or for a backoff that doubles from 1 s to 60 s, so it neither spins nor polls.
- **The breaker.** Home's one breaker counts every job's failures, so five failures of background jobs in a row would refuse interactive calls for up to a minute. The engine needs a small change to home: background jobs' outcomes do not count toward the breaker. If the upstream is down, interactive calls open the breaker on their own, and the kb loop backs off on its jobs' failures.
- **Replies.** The request offers one tool, whose input schema is the kind's reply, and requires the model to call a tool. Each of the four APIs can require a tool call, and with one tool offered that call is the reply. askai already reads the tool calls of each API from a stream (`ai.c`). Anthropic's API refuses a required tool call while extended thinking is on, so there engine jobs run without thinking, or offer the tool unrequired and count an answer without the call as an invalid reply. Where a server can constrain the output to the schema (llama-server's grammars), that is an optimization, and askai validates every reply all the same (§10).
- **Outcomes.** askai keeps the valid items of a reply and counts the invalid ones, so that one invalid item does not lose its valid siblings (srtview's rule). A reply that is invalid as a whole gets one more attempt, under a new id, with the validator's reasons appended; a second invalid reply is recorded in the cache as such. A job that the upstream fails twice (a timeout, a stall, an error) is recorded as failed. Either way, the job is not asked again under this recipe, but for a failed job when demand reaches it again after 7 days, as an outage of the upstream is no answer.
- **Recipes.** A recipe hashes the kind, the prompt, the schema, the model as the upstream names it (llama-server's `/props`, a provider's model, or `--kb-model` for a server that names none), the token limit, the temperature and `--extra`. Its text is stored in the `replies` branch for audit. A new recipe misses the cache. It does not touch the knowledge.

### 8.3 What runs next

The queue holds what to do, not the inputs: a job's kind and what it is about (a file and its window, a term, a pair). It holds at most 4,096 entries, and when it is full the least demanded go. The kb loop gathers a job's inputs (the window's text, the occurrences, the samples) when the job is about to be submitted.

Jobs that answers queued come first, since a question has already shown that their lines matter, taken in turns between the principals whose answers queued them, so that one heavy asker cannot hold all of the background capacity. The rest is ordered by **demand**: counters on terms and files that decay with a half-life of 7 days. Searches feed them (the terms they matched and the files of those terms' occurrences), and so do research (the files that the turns read), gaps (matched terms without claims) and disputes. Each kind of file has a weight, and boundary files come first: configuration, interface definitions, deployment manifests and documentation, where code meets the world. Boundary files that no demand has reached enter the queue as it has room, a source at a time, so that idle capacity reads them. Ties go to the smaller id. A `describe` or `connect` about the same term or pair is not asked again within 30 days, whatever it answered, so that a "none" is not asked again every time demand returns.

The replies and the commit messages record what was read, never why: no question, asker or principal is in them. The queue, which is never stored, keeps the principals of a job only to take turns.

### 8.4 Windows

A window is a run of lines of one file at one blob, at most 12 KiB. Its cut points are chosen by content, as backup tools chunk files: every line may end a window, and a line ends one when a hash of it and the two lines before it meets a condition, at least 2 KiB after the last cut; a window that reaches 12 KiB is cut by force. A forced cut shifts only the window that it ends, because the next cut that content chooses falls where it would have fallen; so an edit re-keys only the windows near it, whatever the indentation of the code. Each window repeats the last five lines of the one before it. Its lines are numbered from 1 within the window, and askai adds the window's offset to the lines that the model cites, so a window whose text is unchanged has the same id wherever the file puts it. A window that only moved within its file therefore keeps its id and is never read again (§8.2), and its claims are moved, not added. A renamed file's windows have new ids, because the path is part of them, and are read again only if demand reaches them. Binary files, files over 1 MiB, files whose lines are too long to cut (minified code) and paths that match the exclusions (vendored code, lockfiles, generated code) are not read, and coverage says so (§6.2).

### 8.5 Mechanical extractors

A later stage adds **rules**: patterns kept in the knowledge base, in the bounded dialect and with their examples, that find kinds of mentions without a model, such as the hosts of URLs, the names of queues and topics in configuration, the tables in SQL and the keys of tickets. They run with `git grep -E` at the current revisions. Their matches form an index that the view consults, and they are never stored. A mention becomes a term when a claim, a question or a contribution names it, so the vocabulary grows with use and not with every string in the estate. A rule may later also read a format whose meaning is fixed, such as a Kubernetes manifest or an OpenAPI file, and yield claims, whose support is `observed` (§3.9).

## 9. Threads, messages and steps

### 9.1 Where work runs

| Thread | Engine work |
|---|---|
| Client loops | Calls of `knowledge`: matching and ranking over the published view, in bounded steps. Calls of `contribute` and `review`: checks of shape, then the contribution posted to the kb loop. The bundle, the finding and the citations of `ask_ai`. No git: what needs git, a client loop asks of the kb loop |
| kb loop (new) | All of the engine's git work, within its own budget of children (§9.3): the reads that calls wait for (cited lines, occurrences, coverage, literals) before those of freshness, jobs and the start; validation and grounding of contributions and replies; the writer; the view, built and published; the job queue and the reply cache; demand. It serves no client, as home serves none |
| Home | The engine's jobs, in the background class, as it runs any job, its breaker leaving them out (§8.2) |

Home could own the knowledge base, but then building views and validating replies would sit between a job's completion and the next job's start. A sidekick of its own keeps home's turns short. The kb loop orders its own steps as any loop does, the work that a call waits for before the background work.

### 9.2 Messages

- **Client loop to kb loop:** `PROPOSE` (a call's contributions or verdict, checked for shape, and the call's handle), `READ` (spans of lines that a call needs, such as an answer's citations), `OCCUR` (occurrences, coverage and literals that a search needs), `FINDING` (an answer's finding, kept unless its asker is anonymous, whose citations of lines queue its `extract` job either way), and `DEMAND`.
- **kb loop to client loop:** `VIEW` (a reference), and `DONE` (by the call's handle: the ids, the lines, the occurrences and coverage, or the reasons for refusing). A handle that has gone stale drops its completion.
- **kb loop and home:** jobs, as between a front and home (`SUBMIT`, `CANCEL`; `DONE`, `PROGRESS`).

Calls that wait for the same lookup share it, as calls for one manual page share its rendering. Demand travels in a buffer that the client loop fills and hands over whole; the kb loop merges it and hands the emptied buffer back, while the client loop fills a second one. Each client loop has its two buffers from its start, so a post needs no allocation, and nothing is shared between the threads (R1, R3). A buffer that fills before it can be handed over drops what does not fit, as demand is advisory.

The kb loop checks a contribution against the view that it holds when it takes the contribution, and merges contributions by identity (§4.2), so two contributions of one term or one claim, from different calls, become one file. Every exchange ends in exactly one completion. Nothing polls, but for one timer: the sources' refs are read every `--kb-check` seconds (60 by default), because nothing pushes a source's new revision to askai and reading a ref costs little.

### 9.3 Bounds

- Replies and files are parsed in slices, as bodies are (`LOOP_PARSE`, `LOOP_VALUES`).
- Matching in askai's process counts under a new unit, `LOOP_MATCH`, with a bound of 64 Ki a step: a letter walked in the trie of names counts one, and a byte that a written pattern is matched over counts 64, its bound of live states (§3.2). Names are matched a word at a time, so a long text spans steps. Written patterns run only over short texts, questions of at most 8 KiB, quotes of at most 4 KiB and the examples of patterns, a line at a time, and a line of more than 1 KiB 1 KiB at a time, each piece resuming where the last stopped (`PCRE2_DFA_RESTART`, with `PCRE2_PARTIAL_SOFT` and `PCRE2_NOTEOL` on every piece but the last, and `PCRE2_NOTBOL` on every piece but the first), which gave the whole line's results in tests. So no call exceeds the bound. Matching over the sources is `git grep`'s, in its own processes.
- Applying a commit to a view counts objects under `LOOP_VALUES`.
- The kb loop runs at most 4 of the process's 16 children at once (`--kb-children`), so that clients' `git`, `man` and `notes` calls keep theirs. Reads of many spans of one source share one `git cat-file --batch`.

### 9.4 Life cycle

The kb loop is a loop as home is, with its own number in the handles: with the engine on, client loops number at most 254. The brainstem starts it after home and before the client loops, which answer `knowledge` as loading until its first view comes. When askai drains, the client loops drain first, their calls ending as in any drain; then the kb loop writes the batch it holds, cancels its jobs without caching them and exits; then home. A contribution written after its call ended does no harm: a retry of it merges into the same file. In a relief of memory pressure, the kb loop gives back its cached occurrences and its kept buffers, as a client loop gives back its kept requests. If askai is killed, it loses at most the contributions of the current batch, whose calls had not answered, so no client was told that they succeeded; jobs whose replies were not yet written are asked again.

### 9.5 Modules

| Module | Role | Uses |
|---|---|---|
| `gitstore.c` | git plumbing shared with the notes: the fixed environment, argv, the settings that sync writes, input of `fast-import`, parsing of `cat-file` output, rules for names, spawning with deadlines, stale locks | `child.h` |
| `kbobj.c` | objects of the knowledge base: canonical files, ids, merges | json-c, libcrypto |
| `kbterm.c` | names: normalization, scopes, the trie and matching, the patterns for `git grep`, the bounded dialect | PCRE2 |
| `kbview.c` | views: persistent maps, changes, digests, references | `kbobj`, `kbterm` |
| `kbquery.c` | `search`, `show` and `related`: ranking, coverage and rendering | `kbview` |
| `kbcheck.c` | validation and grounding of contributions and replies | `kbobj`, `kbterm` |
| `kbjob.c` | job kinds: recipes, prompts, schemas, windows, excerpts, request bodies, parsing of replies | `ai.h`, `kbcheck` |
| `kb.c` | the kb loop: git work, writer, views, queue, reply cache, freshness, coverage, demand | the above, `loop.h`, `job.h`, `child.h` |
| `kbtool.c` | the tools `knowledge`, `contribute` and `review`: a built-in plugin with a part on each client loop | `mcp.h`, `kbquery` |

`kbobj`, `kbterm`, `kbview`, `kbquery`, `kbcheck` and the parsing in `kbjob` do no I/O and start no threads, so tests link them alone. The one new dependency is PCRE2, for its DFA matcher. `git grep -E` needs no PCRE2 in git.

## 10. Validation

Every contribution, every verdict and every item of a reply passes these checks before it becomes knowledge:

| Check | Rule |
|---|---|
| Shape | The schema, the size limits, a relation of the set, a kind of the set, context labels of the configuration |
| Evidence exists | The revision resolves, the lines exist in the file, and askai copies the quote from the blob; output of the `git` tool is never evidence |
| Inside the input | A job's cited lines are within the window or the excerpt that it was shown |
| Grounding | The terms of a claim occur in its cited lines or in the paths of its evidence (§3.3). A new term's name and each new form occur verbatim in the cited lines (srtview's rule that every `SEEN` spelling is on a cited line). A job's context labels are found by askai, not given (§3.1). A value and dates are not checked |
| Patterns | A written pattern is in the dialect, at most 1 KiB, matches no empty string, matches every example that it must match and none that it must not, in PCRE2 and in glibc's `regcomp` alike, and needs no more than the matcher's 64 live states on them. A `connect` pattern matches its examples and finds between 1 and 2,000 lines |
| Secrets | Text that matches a secret rule (keys, tokens, passwords in configuration) is refused in quotes, statements and testimony, and blanked in findings. The rules are patterns in the knowledge base |
| Origin | Contributions and verdicts come only from clients, with their identity; principal 0 cannot contribute. Turns can only read. A job's items are its model's, and its request holds nothing of a conversation |

srtview founded a new term only on two hits in the corpus. A single hit in a transcript is weak evidence, but a single occurrence of a host name in a configuration file is meaningful, so this floor is not kept; grounding does its work.

## 11. Security and privacy

- **Exposure.** Every reader of `knowledge` reads what the knowledge base learned from every source. A role whose upstream may call `git` can learn as much through `ask_ai`'s answers, but a role that is granted `knowledge` and not `git` gains read access to every source, in digested form. Until roles grant sources, grant `knowledge` only to roles that may read every listed source. Then the view will filter claims, findings, occurrences and coverage by the sources they cite or count, so that counts do not reveal what a call cannot see. Every claim records its sources from the start, so that will be a filter, not a migration.
- **Conversations.** What is said in a conversation stays with its asker (§3.5): findings are kept in the asker's own repository, turns cannot write, and jobs are shown sources and shared knowledge only. What others can learn of a question is which jobs it caused: that some lines were read, some terms described and some pairs judged or connected; the replies and the commit messages carry neither question nor asker. Nothing that the upstream writes becomes shared knowledge, so a premise that an asker states, true or false, reaches it only if a person who holds `contribute` publishes it, attributed to them: grant `contribute` to the people who curate.
- **Injected instructions.** Source text and testimony are untrusted input to the model. Jobs offer no tool but their reply. Replies are bound by schemas and validated. Claims carry their quotes, support and attribution, so a reader sees where a claim came from, and people's disputes withdraw it. The upstream cannot write in turns, so text that steers it can change only the answer it gives, which its asker reads with its citations.
- **Secrets.** The secret rules of §10 apply to everything stored: quotes, statements and testimony are refused, and findings blanked. The knowledge base copies quotes from the sources, so without the rules a secret in a source would reach everyone who reads the knowledge base.
- **Attribution.** A client's contributions and verdicts record its identity's name, for accountability, as `git blame` does. Findings stay in their askers' repositories, and askai's log has who asked what.
- **Load.** Matching has limits (§9.3), `related` has its caps, and search output is paged. `contribute` and `review` take at most 64 objects a minute from one principal.
- **The directory.** askai refuses a knowledge base that others than root and its user may change, as it refuses such a configuration file.

## 12. Proof

- **Unit tests** of the modules without I/O: normalization, scopes and matching, the dialect, ids, canonical files and merges, persistent maps and views, ranking, coverage and validation, linked without git or a network, as `tests/buf.c` and `tests/handle.c` are.
- **Determinism:** the same contributions in shuffled orders, with their recorded times shuffled too, give equal view digests.
- **Git:** checks in `tests/askai.py` with real git, as `check_notes` does: contributions, verdicts and refusals, an external edit, a rewound tip, a failed write, packing, the lock, the settings that sync writes on every git that writes, the syncs of directories before an answer, and a clone of the shared repository that holds no finding.
- **The engine end to end** with the fake upstream: canned calls of the reply tool; a cached reply that the mock never sees asked again; identical jobs in flight that reach the mock once; one more attempt with reasons; refusals by the breaker and by a full queue, answered with a backoff and no storm of submissions; freshness after a commit to a fixture source; `ask_ai` with a bundle in its prompt, a finding served at once on level 1 to its asker and never to another, and an answer's citations that queue one job over what they cite, taken in turns between askers. The mock fails the run if a job's request holds any line of a question, an answer or a note.
- **Commit costs:** the measurements of §4.1 and §4.3, repeated in E0 at the size of a real estate and on the deployment's filesystem, set the batch window.
- **Load:** a profile with saturating background jobs and contributions beside the probe, judged by the core's verdict (fairness, units of work, outliers), plus one clause: an interactive call waits behind at most one background job on one slot.
- **Quality:** an evaluation set over a fixture estate: questions and paraphrases of them, with the evidence and the context that answer them. It runs cold and warm, and again after the fixtures' sources change, against a baseline that answers the same questions with scoped `git grep` and one answer call. It measures the precision of citations, the answers that their evidence supports, the share of questions answered from the base, names and contexts wrongly kept apart or joined, and upstream tokens per question, counting every job, background and failed ones included. The engine is worth enabling only where it beats the baseline in total upstream compute at comparable quality. It runs whenever a recipe changes.
- **Metrics** that show whether the cache pays: `ask_ai` calls answered on each level of §6.3, and their turns and upstream tokens; jobs by kind, trigger and outcome (cached, done, refused, failed) and their tokens; contributions and verdicts by type and outcome, refusals by reason; objects by type; claims by support, review and freshness; the view's size and build times; commits and their costs.

## 13. Build plan

| Stage | Work | Gate |
|---|---|---|
| E0 | `gitstore` out of `notes.c` and `mcp.c`, used by both, with the settings that sync writes and the syncs of directories; `kbobj`: canonical files, ids, merges; the layout's commit costs at scale | the notes' checks unchanged; unit tests; equal digests in shuffled orders, times included; commit costs measured on the deployment's filesystem |
| E1 | `kbterm`: normalization, scopes, matching, the patterns for `git grep`, the bounded dialect; `kbview`: persistent maps, built a subtree at a time and published to the loops; the kb loop's number and life cycle; `knowledge` over a knowledge base written by hand, with occurrences and coverage | searches on a fixture; every line that askai's matching accepts is among the lines that `git grep` finds, beyond ASCII too; no step past its bounds; views swapped under load, as `check_reload` does with policies |
| E2 | `contribute` and `review`: claims, terms, testimony and verdicts, lines read by the kb loop, grounding; the view's reading of people's verdicts: entities, merged claims, contested and withdrawn assertions; the writer, with batched commits by author, staging refs and one transaction that moves the tips; the askers' repositories; demand buffers | refusals correct and worded; writes synced and visible on answer; external edits taken in; a rewound tip and a failed write answered; the lock; equal digests with verdicts that replace verdicts |
| E3 | the job runner and `extract`: content-chosen windows, the reply cache, jobs in flight shared, backoff on refusals, home's breaker leaving background jobs out, required tool calls in each API | a cached job never reaches the upstream; background work never delays interactive work beyond G3 (the verdict); an edit re-keys only the windows near it |
| E4 | `ask_ai` with the base: the bundle, private findings, level 1 by equal revisions, lent `knowledge`, citations checked, a job queued over what an answer cites | end to end with the fake upstream; the base fills from answers' citations; no job's request holds a line of a conversation; no finding reaches another asker; on the evaluation set, warm questions cost less total upstream compute than the baseline's at comparable quality (§12) |
| E5 | revisions of sources tracked; freshness; level 1 by unchanged evidence | unchanged, moved, changed and gone on fixture commits; cost in proportion to the changed paths |
| E6 | demand ordering, boundary files, `describe` | answers' jobs still first, in turns between askers; idle reading never delays interactive work |
| E7 | `judge`: nominations, aliases, contradictions in bundles; `recheck` | determinism with models' verdicts; bounded fan-out; no verdict of a model joins an entity |
| E8 | `connect`; rules; sources by role; a saved view, if starts are slow | each its own checks |

E0 to E4 are the smallest useful version: shared knowledge fills from the windows that answers cite, findings serve their askers, and the base serves both back. Until E5, a bundle says at which commit each claim was read, and nothing about whether its evidence is unchanged.

## 14. Open decisions and risks

**Open:**

1. **Senses of a name.** Within a scope, one name is one term, so a service, its repository and its database that share a name are one term, with claims that tell them apart. If that proves confusing, terms can take a qualifier ("payment (table)").
2. **The relations.** Fourteen may be too few, or too many for a small model to choose from reliably. The evaluation set (§12) decides.
3. **Replies.** A required tool call works in every API but Anthropic's with extended thinking; a schema for the response (llama-server's `json_schema`, the Responses API's `text.format`, Gemini's `responseSchema`) constrains more where it exists. Start with the tool call.
4. **Complete answers.** "Which services write `ledger_entries`?" gets the known writers and the coverage behind them. A list kept complete, with the search that supports it and invalidated by any change in its scope, is possible later, if readers need it.
5. **What is deployed.** Context labels come from paths and lines, and configuration shows what is selected, not what runs. Knowing what runs needs a source of the class `observation`, such as an exported inventory; whether askai should read deployment systems itself is open.
6. **What lines depend on.** Freshness follows a claim's own lines, not what they depend on. Marking a claim when the files that define its terms change is possible, if readers trip over meanings that changed under unchanged lines.
7. **Views past 100,000 claims:** quotes read from git when shown, or a saved view (sqlite, as the core foresaw for its blockers, derived and disposable).
8. **Persistent readers.** A `git cat-file --batch` child per source that stays running, if spawning a child for each read shows up in the measurements.
9. **A child that streams.** `child.h` hands a child's output over once all of it has arrived; the start and the lookups of occurrences would hold less memory if the output reached its owner in steps.
10. **Embeddings,** as an optional aid for questions that share no word with the base: never part of an identity and never stored. Only if the vocabulary of terms proves too slow to grow.
11. **A model for each kind of job** (a small one for `judge` and `describe`), which needs askai to have more than one upstream.
12. **Summaries of directories** keyed by git tree ids, which is srtview's pyramid with exact invalidation for free; but every commit changes the tree of every directory above it. Only on demand, if at all.

**Risks:**

1. **The model's quality.** Claims may be shallow or wrong. Quotes, support, disputes, rechecks and the evaluation set catch it; nothing hides it.
2. **Noise in the vocabulary.** Terms come from grounded claims, questions and contributions, not from every string, and common names weigh little in ranking.
3. **Scopes that split.** A model that calls a name a symbol in one window and a service in another makes two terms, one in the source and one in the namespace. People's `same` verdicts join them, and models' make them aliases. Showing a job the terms known in its window would steady the kinds, but would make its id depend on the knowledge base, and read windows again as the vocabulary grows. The evaluation set measures how often it happens.
4. **The cost of the boundary.** A miss pays for its research and then, in the background, for one job that reads what its answer cited (§1.2). If answers cite little, or what they cite yields little, the base fills slowly. The evaluation set measures it against the baseline, and demand and boundary files cover the rest.
5. **Growth of the repository.** A commit rewrites only the trees along its paths (§4.1), every import is a pack that the geometric repacking rolls up (§4.5), the `replies` branch has its retention, and claims grow with meaningful change, since a moved claim makes no new file. Repacking a knowledge base of millions of objects still has to be measured.
6. **One breaker for both classes** until home leaves background outcomes out of it (§8.2).
7. **Complexity inside askai's process.** Every engine step runs under the core's rules, and the stutter harness covers it.

## Appendix A. Words

| srtview | askai | Meaning |
|---|---|---|
| topic, `termN`, `focusN` | term | a name in a scope, and how it is found |
| semantic record | claim | an assertion with its evidence |
| — | assertion | a claim without its evidence: what verdicts judge |
| concept | merged claim | assertions that say the same thing |
| entity | entity | terms that people judged one thing |
| lexicon | aliases | terms that a verdict judged the same, which widen search and join nothing |
| catalog | knowledge base | the git repository of §4 |
| projection | view | the in-memory index of one commit |
| cut | revision | a commit of a source |
| frame lines (OCR) | — | code is text already; documents and testimony are the second witness |
| ground witness | — | not needed: a commit is complete |
| vault, artifact store | reply cache | the `replies` branch, by job id |
| plan, agenda; Facts | job queue; home's scheduler | — |
| heat | demand | decaying counters on terms and files |
| leaf, node, pyramid | — | not carried over (§14) |
| dive | `describe` | a term explained from its occurrences |
| probe, focus | `connect`, then `extract` | a search, then reading what it found |
| terms pass | part of `extract` | one reading yields terms and claims |
| merge, spell | `judge` | one verdict for each nominated pair |
| refinery | the kb loop | — |
| lanes, pacing | classes, slots | home's |

**Words of this design:**

| Word | Meaning |
|---|---|
| alias | a term that a verdict judged the same as another; it widens search one verdict deep and joins nothing |
| assertion | a claim's kind, relation, subject, object and context, without its evidence |
| bundle | what a query returns: terms, claims, findings, occurrences, coverage, gaps |
| claim | an observation of an assertion: its evidence and statement |
| class | what a span's lines can show: code, configuration, document, observation or testimony |
| context | the environments, builds or deployments that an assertion is about, found by askai |
| contested | an object on which people's current verdicts disagree, or an entity whose members a person's verdict separates |
| coverage | what a result searched, and how much of it the knowledge base has read |
| demand | decaying counters that order background work |
| entity | terms that people's verdicts join |
| evidence | lines of a file at a revision, with the quote that askai copied |
| finding | an answer kept for its asker alone, in the asker's own repository, with its validated citations |
| form | one way of writing a term's name |
| freshness | unchanged, moved, changed or gone: of the evidence, never of its meaning |
| job | one request of the engine to the upstream, shown nothing of any conversation |
| kb loop | the sidekick loop that owns the knowledge base |
| namespace | the configured group of sources within which a system's names mean one thing |
| occurrence | a line where a term occurs |
| pattern | a term's search key for `git grep`, made from its normalized name; or one that a person wrote, in the bounded dialect, with examples |
| recipe | the prompt, schema, model and settings of a kind of job |
| review | people's verdicts on a claim's assertion; and the tool that records them |
| scope | where a term's name means one thing: its source, its namespace, or global |
| source | a git repository that askai lists |
| support | how a claim is supported: observed, inferred or stated |
| term | a named thing in a scope: name, kind, forms, pattern |
| testimony | what a person stated, stored as evidence |
| verdict | a judgment about one assertion, or about two assertions or terms; an event, which may replace an earlier one of the same judge |
| view | the immutable in-memory index of one commit of the knowledge base |
| window | lines of one file at one blob, which a job reads |
