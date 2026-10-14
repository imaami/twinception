# Semantic engine design

Design of askai's semantic engine: the knowledge base that `doc/core-design.md` names as the work the core was built to carry ("the semantic archive, search, prompts and schemas"). It is a first design, for review before any code is written; nothing in it is built. Its ancestor in spirit is srtview's engine (srtview's `DESIGN.md`, `semantic_engine.hpp`, `semantic.cpp`, `refinery.cpp`, `topics.hpp`, `rx.hpp`, `agenda.hpp`, `vault.hpp`); §2 says what carries over and what does not.

## Summary

**The problem.** Consider an organization where people ask how something in a large codebase relates to other code and to the world around it: services, data stores, queues, hosts, partner organizations, teams, documents and procedures. Its systems are large and talk to dozens of peers inside and outside the organization. Its LLM capacity is small, so the usual IDE pattern, an assistant that reads the code afresh for every question, costs too much (§1.2). askai can be the endpoint that these questions pass through. The engine makes each answer leave behind knowledge that later questions can use, also questions worded differently: a semantic cache.

**Decisions**, each argued in its section:

1. **Terms are the currency** (§3.2). A term is a name with the spellings that find it, compiled into a pattern that runs over source text at any revision (`git grep -P`), over questions and over claims. This keeps srtview's regular expressions as the currency of knowledge: a pattern is a search key that can be run again, so it finds evidence again after code moves, and it maps differently worded questions onto the same knowledge without asking a model.
2. **Claims are the knowledge** (§3.3). A claim relates a term to a term or to a value through one relation of a small, fixed set, with a sentence and the exact lines that show it. askai copies the quoted lines from git itself and refuses a claim whose lines do not name what it is about. Words that a model writes are never evidence.
3. **Git holds what cannot be computed again** (§4): terms, claims, verdicts, answers and what people state, in one repository with one writer, as plain files that a person can read, diff and blame. Everything mechanical (occurrences, freshness, indexes, demand) is computed again when needed and never stored.
4. **Reads come from an immutable view** (§5): an in-memory index of one commit of the knowledge base, published to the client loops by reference as the policy is, and updated incrementally. Serving what is known touches neither git nor a model; only raw occurrences, which no claim explains yet, are looked up with `git grep`.
5. **Mechanical work first, the client's model second, the upstream last** (§6). The server finds candidates mechanically. The client's own model, which knows the asker's context and runs on the asker's hardware, judges what applies. The upstream is spent on research, and on background reading that fills only idle capacity.
6. **Every model job is content-addressed** (§8). A job's id hashes its kind, its recipe and every byte the model is shown. The model reads a window of a file once for all users, until the window or the recipe changes, and identical jobs in flight are one job.
7. **It fits the core** (§9). One new sidekick loop owns the repository, the view and the job queue. The tools run on the client loops against the published view, model work goes to home as background jobs, git runs as supervised children, and CPU work runs in bounded steps.

**The smallest useful version** (§13, stages E0 to E3) runs no background jobs at all. The knowledge base fills from the research that `ask_ai` calls already do: the upstream records what it learns with `contribute`, askai checks it against the sources, and the next ask starts with it. Background reading, consolidation and freshness come in later stages.

## A question's path

1. Alice's assistant calls `knowledge` with "what sends files to Contoso?". The view matches the term Contoso, a carrier, which is an organization whose spellings are `Contoso` and `CTSO`. No claim is about it yet, so the result lists its occurrences at the tracked revisions, among them `contoso_sftp_host` at `shipping:config/prod/sftp.yaml:4` and a line of `shipping:src/ship/upload.go:41`, and says that nothing is known about what sends to it.
2. The assistant calls `ask_ai`. askai adds that result to the prompt. The upstream reads `upload.go` and the CronJob that runs it with `git`, then calls `contribute` twice: "shipment-uploader `sends_to` Contoso" and "shipment-uploader `runs_on` prod-batch", each citing the lines that show it. askai reads those lines from git, checks that they name both sides, and commits both claims. The answer cites the claims, and askai keeps it as a finding.
3. Bob asks his assistant "how do we deliver shipments to the carrier?". No term matches, but "shipments" meets "shipment" in the claim's statement (§6.2 compares words by their first six characters), the word is rare in the base, and the claim ranks first. Bob's assistant answers from the claim and its quoted evidence, at no cost to the upstream.
4. A week later `upload.go` changes. When the kb loop sees the new revision, it finds that the claim's evidence changed and marks the claim as changed. Answers that cite the claim say so. The next question that touches it puts a recheck job in the queue.

## 1. Problem and constraints

### 1.1 The questions

The questions come from people who work on or around the systems, through their assistants:

- *Locating:* where is the retry policy for partner callbacks configured in production?
- *Relating:* which services write the `order_lines` table, and who owns it?
- *Explaining:* what is OMS in this codebase?
- *Impact:* if the `weight_grams` field of `ShipmentRecord` changes, what breaks outside this repository?
- *The world around the code:* what happens when Contoso's delivery report is late, and whom do we contact?

Most of these questions are about the relations between named things: symbols, services, stores, channels, hosts, environments, external organizations, teams, documents and procedures. Some of the answers are in the code, some in configuration and documents, and some only in people's heads.

### 1.2 The budget

As an illustration, take a modest deployment where an upstream slot processes prompts at 1,500 tokens per second and generates 30 tokens per second. The conclusions hold within a factor of a few.

- **Reading everything is out of reach.** A million lines of code is about 40 MB, or about 3,300 windows of 12 KiB. A window is about 3,000 tokens of prompt and about 500 tokens of claims, which takes about 2 s plus 17 s, so about 19 s per window. That is about 17 slot-hours per million lines for each version of the prompt. An estate of 20 million lines takes two weeks of one slot. Reading the whole estate is not a goal. Reading follows demand, and starts at the boundaries where code meets the world (§8.3).
- **Keeping up is cheap.** Jobs are keyed by content (§8.2). If 1 % of the files change in a day, reading the changed windows costs about 10 minutes of a slot.
- **The IDE pattern is the expensive one.** An assistant that researches a question reads tens of thousands of tokens over several turns, which is 20 to 60 s of a slot before it writes anything. A few hundred such questions a day take eight hours of a slot. Questions overlap heavily, so the knowledge base turns a repeated question into a bundle of a few thousand tokens. The client's model reads the bundle, or the upstream reads it in one turn.

### 1.3 What askai already has

- An MCP endpoint with identities, principals and roles. A role grants tools to clients, and its `upstream` member grants tools to the upstream in the turns of the role's ask calls.
- `ask_ai` with turns: the upstream may call the tools that its role lends it (`git`, `man`, `notes`, `compact`, `background`, `jobs`, `wait`, `timer` and the plugins' tools) until it answers.
- The `git` tool: read-only, allowlisted commands in the listed repositories, `grep -P` among them.
- The `notes` tool: private notebooks of the upstream and of clients, each thread a bare git repository, each note a file, written by `git fast-import` and never changed.
- Home's scheduler: upstream slots, deficit round-robin across principals, an interactive and a background class (background jobs hold all slots but one, and with one slot interactive jobs preempt them), a breaker, streamed answers and progress.
- Tasks (CPU work in slices of 250 µs, placed on the lighter of two loops), children (pidfd, deadlines, a cap of 16 at once), blockers, and the stutter harness, whose verdict fails a run on unfairness, unbounded work and gross outliers.
- Plugins that a configuration file loads. A loaded plugin may start no process or thread and cannot reach home, so the engine is built in, as `man` and `git` are.

### 1.4 Goals

- **G1** A question asked again, in the same or other words, costs no research while the knowledge exists and still holds.
- **G2** Everything served points to evidence that a person can check, and says whether that evidence still holds.
- **G3** Interactive work goes first. Background work uses only idle capacity, and no input is asked of the model twice.
- **G4** Knowledge survives restarts, changes of model and prompt, and changes of code, where it finds its evidence again.
- **G5** The engine keeps the core's rules: bounded steps, one owner for each object, no polling, no stutter.
- **G6** A person can read, diff and blame the knowledge base with git.

### 1.5 Non-goals

- Complete coverage of an estate (§1.2).
- A code search engine. `git grep` is the search engine, and the engine makes its patterns better.
- A wiki or a web interface. The MCP tools render the knowledge, and clients present it.
- Embeddings in the first version (§14).
- Deciding what is true. The engine records what the sources and people say, with evidence and attribution, and shows disagreement.

## 2. From srtview: what carries over and what does not

### 2.1 What carries over

| srtview | Why it matters here | askai's form |
|---|---|---|
| Content identity everywhere: a task's id is a hash of its inputs, and a cache travels to another machine | It deduplicates model work across users and time | Job ids (§8.2), git blob ids, the reply cache |
| Model output validated mechanically: citations inside the window, spellings on the cited lines, quotes copied from the source and never from the model | It is the guard against invented facts | §10 |
| Regular expressions as the currency: named topics, composition with `\{name:}`, spellings knitted into one pattern (`rx::knit`) | Patterns are search keys that run at any revision | Terms (§3.2) |
| The projection as a function of the stored facts, independent of the order of completions (`DESIGN.md` I8), checked by a digest (`dirHash`) | Views are reproducible and testable | The view and its digest (§5) |
| Records as triples with a sentence and cited spans; byte-identical twins as one concept; judged pairs (same, related, contradicts, novel); entities as names that verdicts join | It is a workable shape for knowledge between named things | Claims, verdicts, entities (§3.3, §3.4) |
| "A verdict nobody reads was not worth asking for": contradictions travel with the claims they contradict | Model work is spent only where something reads its result | Bundles (§6.2), nominations (§3.4) |
| A probe proposes a search, the corpus answers it, and the write sees only what the search found | A cheap job produces a mechanical query, and the costly reading is of real evidence | The connect job (§8.1) |
| Recipes: prompt, schema, model and settings in the identity of a cached reply | A changed prompt asks again, and an unchanged one never does | Recipes (§8.2) |
| Bounded fan-out (four judgment candidates, three focus partners), capped retries with feedback | The model's work per new fact is bounded | §3.4, §8.2 |
| Retrieval weighted by how rare a word is in the corpus, with no word lists | It works in any language and needs no model | Ranking (§6.2) |
| Heat from user actions steers the background work | Work follows what people ask | Demand (§8.3) |
| "Waiting is a dependency"; workers report and never coordinate; one owner serializes scheduler and projection state (I7) | It keeps concurrency simple | The kb loop (§9) |

### 2.2 What does not carry over

| srtview | Its problem | The rule here |
|---|---|---|
| Two ontologies, topic regexes (terms, dives, focuses) and semantic records, joined by a side channel (the lexicon) | Knowledge about one thing lives in two systems | One model: a claim's subject and object are terms (§3) |
| The projection rebuilt from scratch on every landing, on the UI thread: a 3.4 s refold per landing, measured [S] in `doc/core-design.md` | Work proportional to all state, for every event, on the serving thread | The view changes incrementally, off the serving loops, and is published by reference (§5) |
| Pokes, `landed()` counters swept per tick, harvest budgets of four per tick, a 1 ms timer that scans one video per tick, retire sets, the seams before and after a cut | Many small mechanisms decide when work happens | Each job ends in exactly one completion, a message to one owner (core R6), and work runs in bounded steps (core §1) |
| Mutable state shared between threads behind several mutexes (the executor's, the poke's barrier), with file I/O under a lock | Lock order and lifetime bugs, and stalls | One owner for each object (R1), immutable views (R5), no lock across I/O (R7) |
| Reply grammars of free text (`TERM:`, `SEEN:`, `REGEX:`, `MERGE:`, the last `SAME` or `DIFFERENT` of a reply) parsed by heuristics | Formats break silently | Each job answers with one call of one tool whose arguments a JSON schema fixes, and askai validates them (§8.2) |
| A catalog keyed by the recipes and the set of window ids: a new prompt, model or frame starts an empty view | Knowledge disappears with every prompt change until the corpus is asked again | A claim's identity does not include the recipe. Recipes key only the reply cache, and appear in provenance (§3.7) |
| State of the session rebuilt at every start by replaying cached replies | Startup cost and logic that replays | The knowledge base persists, and the view is built from its commit (§5) |
| Witnesses and cuts that keep asks from running over a half-read corpus | Machinery for a source that is read gradually | Sources are git commits, complete by construction |
| 64-bit truncated ids | Collisions in a large shared base | 128-bit ids (§3.7) |
| A JSON parser and encoder written by hand | Code to maintain | json-c and `jb.c`, as askai has them |
| Three votes for each pair of spellings, and asks about the whole term directory | Costly rituals for misheard speech | One judgment for each nominated pair. Code is not misheard, and its names vary systematically, so most aliasing is mechanical (§3.2) |
| A summary pyramid over every video | Costly, and of little value for the questions in §1.1 | None by default (§14) |
| Thermal pacing and lanes inside the executor | Scheduling policy in two places | Home's slots, classes, fairness and breaker |
| Cut, witness, leaf, node, dive, probe, focus, heat, refinery, vault, scribe, loom, braid | Names that need explaining | Plain words (Appendix A) |

## 3. The model

### 3.1 Sources, revisions and evidence

A **source** is a git repository that askai lists with `--git`, named as the `git` tool names it. Each source has a tracked ref, `HEAD` unless configured otherwise, whose commit is its current **revision**. Everything is a git repository: documents from other systems (wiki pages, tickets, runbooks, API catalogs) enter by being exported into a repository that askai lists, and what people state is stored in the knowledge base itself (§3.6).

**Evidence** is a range of lines of a file at a revision: the source, the commit, the path, the first and last line, the blob, and the **quote**, which is the text of the lines as git gave them to askai. A claim has at most 8 spans of evidence, each at most 60 lines and 4 KiB. A span of testimony names the testimony and its lines instead of a source and a path.

### 3.2 Terms

A **term** is a named thing:

| Field | Meaning |
|---|---|
| `name` | The name as first written, such as `OrderService` |
| `kind` | A hint: service, component, symbol, file, store, channel, host, environment, organization, team, document, process or concept |
| `spellings` | The literal strings it is written as: `OrderService`, `order-service`, `ordsvc` |
| `pattern` | Optional: a regular expression that a person wrote, which may refer to other terms as `\{name:}` |
| `by` | Who added what: jobs and identities, with times |

**Identity.** A term's identity is its normalized name: the name split into words at changes of case, between letters and digits, and at `_ - . / :` and spaces, case-folded, and joined by single spaces. `OrderService`, `order-service` and `ORDER_SERVICE` are one term, "order service". Two different names for one thing (`ordsvc`, OMS, "the order backend") are two terms that a verdict joins into one entity (§3.4). The same name for a service, its repository and its database is one term, and claims tell the facets apart; §14 keeps a split of senses open.

**Matcher.** askai compiles each spelling into a pattern that knows identifiers: the spelling's words case-insensitively, an optional separator between them, and a match that begins and ends at a non-alphanumeric character or at a hump of camel case. So "order service" finds `OrderServiceClient`, `order_service.yaml` and "the order service", but not `reorderService`. In prose (questions and statements), the last word may take a suffix: plurals, and the inflections of languages such as Finnish ("Pythonin" for Python), as srtview's `termMatcher` allowed. A term's matcher is the union of its spellings' patterns, knitted into one pattern (`rx::knit`), and of its written pattern. The spellings are stored, and the pattern is derived from them. srtview stored the knitted pattern and needed `rx::unknit` to open it again; storing the set makes that unnecessary.

**Where matchers run:** over source text at a revision with `git grep -P`, which gives the term's **occurrences**; over questions, which gives the terms that a question mentions (§6.1); and over quotes, which grounds claims (§10).

**Limits.** At most 32 spellings and a written pattern of at most 1 KiB, compiled by PCRE2 with match limits, without backreferences or recursion. Its references resolve without cycles, as in srtview's `topics.hpp`. A matcher that finds more than 10,000 lines across the sources is refused as unspecific.

### 3.3 Claims

A **claim** is one assertion with its evidence:

| Field | Meaning |
|---|---|
| `kind` | `fact`, `rule` (a constraint or a policy), `step` (a step of a procedure) or `definition` |
| `subject` | A term |
| `relation` | One of the fixed set below |
| `object` | A term, or a `value`: a literal such as "02:00 every night" |
| `detail` | The source's own phrase for the relation, at most 128 bytes: "uploads shipment files over SFTP to" |
| `statement` | One sentence that stands alone, at most 1 KiB |
| `evidence` | Spans (§3.1) |
| `by` | Who asserted it: jobs and identities, with times |

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
| `runs_on` | A runs on, or is deployed to, B | runs |
| `configured_by` | A's behavior is set by B: a file, a setting | configures |
| `owned_by` | A is owned, maintained or operated by B | owns |
| `documented_in` | A is described in B: a document, a ticket, a decision | documents |
| `replaces` | A replaces B | replaced by |
| `has` | A has the property B, usually a value | — |
| `relates_to` | Anything else; `detail` says how | relates to |

The set belongs to the format (§4.2). A new relation needs a new version of the format, and old files stay valid under it.

**What a claim may be about.** A claim's subject, and an object that is a term, must be something that the source names: its matcher must find it in the cited lines, or, for a claim read from a window, in the window (§10). Things that the source does not name, such as "the retry mechanism", can live in statements but cannot be subjects. This keeps claims anchored to names that the matchers can find again.

### 3.4 Verdicts, entities and merged claims

A **verdict** is a judgment about two objects, `same`, `related`, `contradicts` or `unrelated`, or about one object, `confirm` or `dispute`, with a reason, by a job or by a person. The view takes a person's verdict over a job's, and among verdicts of one rank the latest. The times are stored in the files, so the outcome depends only on the set of verdicts.

**Entities.** Terms that a `same` verdict joins form one entity. Its title is the name of its earliest term, and ties go to the smaller id.

**Merged claims.** Claims with the same kind, relation, subject entity and object (an entity, or a folded value) are one merged claim, mechanically: the same assertion seen at two places, or seen again at a later revision. This is srtview's rule that one claim seen in two places is identity, not a judgment. A `same` verdict merges other claims. A merged claim lists the evidence of all its claims, and its statement is chosen by a fixed rule. A claim that contradicts it travels with it in every bundle.

**Nominations.** Pairs are never judged wholesale. A new claim is judged against at most three claims that share its subject and overlap it most in words. A new term is judged against at most two terms whose names share a word, or whose initials match it as an abbreviation does (OMS and "order management system"). Nominations wait for demand (§8.3): a pair that nobody asks about is not judged.

### 3.5 Findings

A **finding** is an answer that `ask_ai` gave and askai kept: the question and its system text, the digest of the bundle that the upstream was given, the answer, the citations that askai could validate (claims, and spans of sources), the terms that the question matched, the commits of the knowledge base and of the sources it was answered at, and the recipe. Who asked is recorded only if the configuration says so (§11). A finding is an event, not a fact. It is offered as an earlier answer, with the freshness of what it cites.

### 3.6 Testimony

**Testimony** is what a person states through their client with `contribute`, at most 8 KiB of text. It is stored as a file in the knowledge base, attributed to the identity, and claims can cite its lines as they cite a source's (§3.1). Testimony is how knowledge that is in no repository gets in: contracts, schedules, contacts, the reasons for decisions. A turn of an ask call cannot testify, because the model has no testimony to give. A client's testimony is attributed to the client's identity, which answers for it whether a person typed it or the person's assistant did.

### 3.7 Identity

| Object | Its identity hashes | Not hashed |
|---|---|---|
| Term | its normalized name | the name as written, kind, spellings, pattern, provenance |
| Claim | kind, subject, relation, object (a term, or a folded value), evidence (source, path, blob, lines) | statement, detail, quotes, provenance |
| Verdict file | the ids it is about, sorted | the judgments it holds |
| Finding | question, answer, citations | everything else |
| Testimony | identity, text, time | — |
| Job | kind, recipe, every byte the model is shown | — |

An id is the first 128 bits of SHA-256, over a tag naming the type and the canonical fields, written as 32 hexadecimal digits. The tools take any unique prefix of at least 8 digits. A claim's identity includes its evidence, so a claim is an observation. The same assertion at a new revision is a new claim, which merges with the old one (§3.4). A claim stays true of the commit that it cites.

### 3.8 Freshness

At a source's current revision, each span of evidence is:

- **fresh**: its blob is unchanged, or its quote is still at its lines;
- **moved**: its quote is found verbatim elsewhere, because lines shifted or the file was renamed;
- **changed**: its quote is gone, but the claim's terms are still found in the file;
- **gone**: none of these.

A claim's freshness is that of its best span. A moved claim keeps its file: the view records where its quote is now, and nothing is asked or written. When a tracked ref moves, the kb loop asks `git diff-tree` for the paths that changed, and checks only the spans in those paths, each with a read of the new blob and a search of its bytes. No model is asked. Freshness is not stored: it is a function of the knowledge base and of the sources' revisions, and cheap to compute again. A changed claim is a candidate for a recheck job (§8.1) when there is demand for it. A gone claim is history: still true of its commit, and offered last.

## 4. The knowledge base in git

### 4.1 Layout

```text
KB.git                         bare; --kb DIR, made if it does not exist
  refs/heads/knowledge
    FORMAT                     the version of the format, and its relations
    terms/3f/3f9a...e1.json    one file per term
    claims/5d/5d1e...07.json   one file per claim
    verdicts/a0/a0c4...9b.json one file per judged object or pair
    findings/0c/0c4d...11.json one file per kept answer
    testimony/7e/7e21...5a.md  one file per statement, as given
  refs/heads/replies
    extract/4c/4c1f...08.json  each job's reply and outcome, by job id
    recipes/91/91be...2d.txt   each recipe's prompt, schema and settings
```

The first two digits of an id name its directory, so every tree stays small, and `FORMAT` can change the fan-out if the base passes a million objects.

### 4.2 Files

Each file is canonical JSON with its members in a fixed order, one member per line, so that a diff reads as a change of fields. A file's content is a function of the set of contributions to its object: provenance and spellings are sets, kept sorted, and each single field follows a fixed rule (a person's value over a job's, then the earliest, then the smaller id). The writer rewrites a file when a contribution adds to it. The order in which contributions arrived therefore never shows in the tree, and the view built from the tree depends only on what was contributed (srtview's I8). Provenance keeps at most 16 entries, chosen by the same rule.

A claim file:

```json
{
"format": 1,
"type": "claim",
"kind": "fact",
"subject": "3f9a01c24e5b8d77a6c1e0f2b9d4c3e1",
"relation": "sends_to",
"object": "91be07aa3c5d4f6e8a9b0c1d2e3f4a5b",
"detail": "uploads the daily shipment file over SFTP to",
"statement": "The shipment-uploader job uploads the daily shipment file to Contoso's SFTP server.",
"evidence": [
{"source": "shipping", "commit": "1a2b...", "path": "src/ship/upload.go", "blob": "77c0...", "lines": [38, 47], "quote": "..."}
],
"by": [
{"job": "4c1f...", "recipe": "91be...", "at": "2026-10-14T13:02:11Z"},
{"identity": "alice", "via": "upstream", "at": "2026-10-14T13:05:40Z"}
]
}
```

### 4.3 One writer

The kb loop is the only writer. It gathers accepted contributions for at most 50 ms, or until it has 256 objects, and commits them as one commit made by `git fast-import` from the tip of the branch, as the notes are; the notes measure 3.9 ms a commit (doc/core-measurements.md, S10). The author is the contributing identity's name, or askai for jobs. The message is plain, with trailers that name the job or the principal. If the tip has moved, an external edit came in between: the kb loop takes that edit in (§4.5) and commits again. A lock file in the directory refuses a second askai that would write the same knowledge base.

### 4.4 What is not stored

Occurrences, freshness, indexes, scores, demand and the mentions that rules find (§8.5) are not stored, because they can all be computed again from the knowledge base and the sources. Storing them would make the repository churn with every commit of every source, and would make the view depend on history.

### 4.5 Operations

- **Packing.** `git fast-import` writes small imports as loose objects (`fastimport.unpackLimit`). The kb loop runs `git maintenance run --auto` as a child after every 256 commits.
- **Retention.** The `replies` branch is a cache. It can be squashed into one commit, or pruned by age, without touching the `knowledge` branch.
- **Replication.** The repository can be cloned and fetched. A view builds from any commit, so a read-only askai can serve `knowledge` from a mirror.
- **External edits.** A curator may edit the `knowledge` branch directly, for example to fix a term's name or kind. When the kb loop finds a tip that it did not write, it validates the changed files as contributions (§10), applies the valid ones to the view, and logs the invalid ones without deleting them. Deleting a file does not withdraw a claim, because a cached reply would add it again; a person's `dispute` withdraws it, and stays.
- **Audit.** `git log -- claims/5d/5d1e...07.json` shows who added, judged or disputed a claim, and which job and recipe produced it.

## 5. The view

The **view** is an immutable, in-memory index of one commit of the `knowledge` branch, with a layer of freshness for the sources' current revisions. It holds:

- terms by id, and an index from a short prefix of each spelling's first word to the terms, for matching questions;
- compiled matchers, for the terms that have written patterns;
- entities (a union-find over terms) and merged claims (a union-find over claims), whose representatives follow fixed rules, so the order of unions does not matter;
- claims by id, and postings from each entity to its claims by relation and direction;
- verdict edges, and findings by term;
- word statistics (how many statements, questions and names each word occurs in) for ranking;
- the freshness of each claim at the current revisions.

**Publication.** The kb loop builds a new view and posts a reference to every client loop. A loop takes the new view for calls that begin from then on and lets go of the old one, and calls under way keep the view they began with. This is how the policy is reloaded (core §6, R5). Publications coalesce, at most one every 100 ms.

**Incremental change.** The view is split into 64 shards by id. A commit rebuilds only the shards that it touches, and the new view shares the other shards with the old one by reference. The union-finds only grow as objects are added; a retraction rebuilds them in bounded steps, off the serving path.

**Determinism.** A view's digest is a hash over its canonical dump, and it depends only on the tree of the knowledge base. A test commits the same objects in shuffled orders and compares the digests. This is srtview's `dirHash`, made a gate.

**Size.** A claim takes about 1 KiB in the view, so 100,000 claims take about 100 MB. Beyond that, quotes and statements can stay in git and be read when shown, or the view can be saved next to the repository for fast starts, but never as the authority (§14).

**Start.** The first view is built from the branch's tip: `git ls-tree -r` and `git cat-file --batch` run as children, and their output is parsed in slices as tasks. Until the view is published, `knowledge` answers that the knowledge base is loading.

## 6. Retrieval and the semantic cache

### 6.1 From words to terms

The text of a query is split into words and folded, identifiers included, so `OrderService` in a question becomes "order service". The first four characters of each word, or the whole word if it is shorter, look up the terms whose spellings begin with a word that starts the same way, and those terms' matchers, in their prose form, confirm or reject each candidate; the matchers of terms with written patterns run over the text as well. Matched terms expand to their entities. An identifier in the question that no term matches (camel case, snake case, a path) is looked up in the sources as a literal: a hit is offered as a candidate term, which the client can add with `contribute`.

### 6.2 Bundles

A **bundle** is what a query returns:

- **Terms** matched, with their kinds, entities, spellings and definitions.
- **Claims**, ranked: claims that link two of the matched terms first, since they answer relationship questions; then by the rarity of their shared terms and the overlap of their statements with the query, both weighted by how rare each word is (srtview's `vocab`); fresh claims before changed ones, and gone ones last; confirmed claims up and disputed claims marked. A claim that contradicts a ranked claim comes with it. Words are compared by their first six characters, so "shipments" meets "shipment" and "Pythonin" meets "Python". srtview compared whole words; the cut needs no word list either, and it tolerates plurals and the inflections of languages such as Finnish.
- **Findings** whose terms and questions overlap the query, with the freshness of what they cite.
- **Occurrences** of matched terms that no claim explains: lines from `git grep -P` at the current revisions, capped. They are raw material that the client's model can read itself.
- **Gaps**: matched terms that have no claims, and pairs of matched terms that no claim connects.

Output is text, paged with `offset` as the `git` and `notes` tools are, and in a turn within a window of an eighth of the context.

### 6.3 Three levels of reuse

1. **The same question over the same knowledge.** An `ask_ai` call whose normalized prompt, system text, recipe and bundle digest equal those of a kept finding, all of whose citations are fresh, gets that finding at once, marked as such. At temperature zero the answer is a function of exactly these inputs; this is srtview's identity of an answer. A call that gives `fresh: true` skips this level, and so does a call that goes on with a notes thread. By default a finding is offered only to its asker (§11), so this level serves other askers only where findings are shared.
2. **A different question about the same things.** The bundle carries the claims and earlier findings, and the client's model or the upstream answers from them. The upstream often answers in one turn without calling a tool.
3. **Nothing known.** The upstream researches with `git` and records what it learns. The next question about it lands on level 2.

### 6.4 Who judges relevance

The server does not decide that two different questions are the same question. A wrong hit costs more than a miss, and judging equivalence with a model would spend the call that the cache was meant to save. The server matches mechanically and shows its candidates with their evidence and freshness. The client's model judges what applies: it knows the asker's context, which the server does not, and it runs on the asker's hardware, so its reasoning costs the upstream nothing. The cheap reasoning thus moves to where it is free, and the scarce upstream is kept for research.

## 7. Tools

Two tools, so that a role can grant reading and writing separately; roles grant tools, not actions. Neither tool speaks of git: they name terms, claims and evidence, as the `notes` tool names notes and not commits. Git is how the knowledge is kept, not what a caller works with.

### 7.1 `knowledge`

Read-only, lent to turns when the role's `upstream` member grants it (`TOOL_LENT`), paged (`TOOL_PAGED`).

| Action | Arguments | Result |
|---|---|---|
| `search` | `text`: a question, names or identifiers | a bundle (§6.2) |
| `show` | `id` or `name` | the whole object: a term with its spellings, matcher, entity, claims by relation and top occurrences; a claim with its quotes, freshness, verdicts and history; a finding with its citations |
| `related` | `name`, optional `relation`, `depth` of at most 3 | the graph around a term: paths of claims, for impact questions |

### 7.2 `contribute`

Writes, lent to turns when the role's `upstream` member grants it.

| Action | Arguments | What askai does |
|---|---|---|
| `claim` | `subject`, `relation`, `object` or `value`, `statement`, optional `detail` and `kind`, `evidence`: `[{source, path, lines, rev}]`, or `{testimony, lines}` for testimony | resolves `rev` (default: the current revision), reads the lines with one `git cat-file --batch`, copies the quote, checks the claim (§10), and answers with the claim's id and the terms that it found or made, or with every reason for refusing it, worded so that a model can correct the claim and try again |
| `term` | `name`, `kind`, `spellings`, optional `pattern` and `evidence` | checks the spellings and the pattern, and adds or extends the term |
| `testimony` | `text`, optional `about`: names | stores the text, attributed (§3.6), and answers with its id and the address that claims cite it by; clients only, never turns |
| `verdict` | `ids` (one or two), `value`, `reason` | records the judgment |

A write is durable when its call answers: the reply waits for the commit of its batch, at most 50 ms plus a few ms for the commit.

### 7.3 In `ask_ai`

With `--kb`:

- Before the first turn, askai runs a search over the prompt. If a kept finding matches on level 1 (§6.3), askai answers with it at once. Otherwise it adds the bundle to the prompt in brackets, as it adds notes, within an eighth of the context and `--kb-bundle` (16 KiB by default).
- Turns get `knowledge` and `contribute` as the role's `upstream` member grants them.
- After the answer, askai checks the citations in it (`[c:5d1e9b20]` for a claim, `[shipping:src/ship/upload.go:38-47]` for lines) and keeps a finding with the valid ones. The bundle shows every claim in the form that cites it, and `contribute` answers with that form, so the upstream sees it before it answers. The trace that the client receives says what was recorded.
- The files that the turns read with `git` add to demand (§8.3).

A configuration that lets clients read the knowledge base and lets only the upstream write to it, with evidence that askai checks (the `upstream` member landed in 9bd5f42):

```json
{
  "roles": {
    "user":    { "tools": ["ask_ai", "knowledge"],
                 "upstream": ["git", "knowledge", "contribute", "notes"] },
    "curator": { "tools": ["ask_ai", "knowledge", "contribute"] }
  }
}
```

### 7.4 Notes stay separate

The `notes` tool stays as it is: private, per thread, unvalidated, the upstream's working memory. The knowledge base is shared, validated and attributed. Merging the two would either weaken the knowledge base's checks or burden the notebook with them. They share plumbing instead: the code in `notes.c` that runs git in a fixed environment, builds the input of `git fast-import`, parses the output of `git cat-file` and removes stale locks becomes a module that both use (§13, E0).

## 8. Background jobs

### 8.1 Kinds

| Kind | The model is shown | It answers | Queued when |
|---|---|---|---|
| `extract` | a window: source, path, numbered lines | terms with spellings, and claims citing lines of the window | there is demand on its file, or the file is a boundary file (§8.3) |
| `describe` | a term and up to 24 KiB of its occurrences, in a fixed order | a definition and other claims citing those lines | a term that someone asked about has no definition |
| `judge` | two claims with their quotes, or two terms with their claims or occurrences | a verdict and its reason | a nominated pair (§3.4) has demand |
| `connect` | two terms and up to 8 KiB of the occurrences of each | a pattern to search for code that involves both, or none | a question relates two terms that no claim connects |
| `recheck` | a changed claim and the current text where its evidence was | whether the text still supports it, and the lines that do | a changed claim (§3.8) has demand |

`connect` is srtview's probe, search and focus in one chain: its pattern is validated (§10) and run with `git grep -P` at the current revisions, and the windows around the hits become ordinary `extract` jobs, which the reply cache serves like any other. srtview's focus write is thereby replaced by ordinary reading. A `recheck` that finds support yields a new claim with new evidence, which merges with the old one. One that does not yields a `dispute` verdict by the job: the old claim stays true of its commit.

There is no answer kind, because `ask_ai` is how answers are made. There are no summaries by default (§14).

### 8.2 One runner

- **Identity.** A job's id is a hash of its kind, its recipe and every byte that the model is shown, and of nothing else. srtview kept a video's title out of the identity because a renamed video is the same video. Here the path is shown and hashed, because a configuration file under `prod/` and the same file under `staging/` say different things.
- **Reply cache.** The `replies` branch, which the kb loop indexes at start, records each job's reply and outcome: the ids it added, or none. A job whose outcome the knowledge base already holds is not run again. A job whose reply is cached but whose outcome the knowledge base lacks (a knowledge base rebuilt, or the cache carried to another deployment) is validated again from the cached reply, without an upstream call. A job that is in flight with the same id takes the new demand, and nothing is submitted twice.
- **Submission.** Jobs go to home as fronts' jobs do, in the background class, at most `--kb-jobs` at a time (2 by default; 0 runs none). Home's classes keep a slot for interactive work, and with one slot an interactive call preempts a background job.
- **Replies.** The request offers one tool, whose input schema is the kind's reply, and requires the model to call a tool. Each of the four APIs can require a tool call, and with one tool offered that call is the reply. askai already reads the tool calls of each API from a stream (S10), so this needs no plumbing for structured output in each API. Where a server can constrain the output to the schema (llama-server's grammars), that is an optimization, and askai validates every reply all the same (§10).
- **Outcomes.** askai keeps the valid items of a reply and counts the invalid ones, so that one invalid item does not lose its valid siblings (srtview's rule). A reply that is invalid as a whole gets one more attempt, under a new id, with the validator's reasons appended. A second failure is recorded in the cache, so that the job is never asked again under this recipe. A failure of the upstream (busy, breaker, timeout) records nothing, and the job waits for its turn again.
- **Recipes.** A recipe hashes the kind, the prompt, the schema, the model, the token limit and the temperature. Its text is stored in the `replies` branch for audit. A new recipe misses the cache. It does not touch the knowledge.

### 8.3 What runs next

The kb loop orders the queue by **demand**: counters on terms and files that decay with a half-life of 7 days. Searches feed them (the terms they matched and the files of those terms' occurrences), and so do research (the files that the turns read), gaps (matched terms without claims) and disputes. Each file kind has a weight, and boundary files come first: configuration, interface definitions, deployment manifests and documentation, where code meets the world. Ties go to the smaller id. No dependency graph is needed, because the kb loop gathers each job's mechanical inputs (windows, occurrences, samples) before it queues the job.

### 8.4 Windows

A window is a run of lines of one file at one blob, at most 12 KiB and 300 lines. Its cut points are chosen by content, as backup tools chunk files: a line that may begin a window (one at the file's outermost indentation, after a blank line) becomes a cut point when a hash of its text meets a condition, or when the window would grow past its limit. An edit thereby moves only the cut points near it, and the windows away from it keep their text, their ids and their cached replies. Each window overlaps the one before it by a few lines. Its lines are numbered from 1 within the window, and askai adds the window's offset to the lines that the model cites, so a window whose text is unchanged has the same id wherever the file puts it. A window that only moved within its file therefore keeps its id and is never read again (§8.2), and its claims are moved, not added. A renamed file's windows have new ids, because the path is part of them (§8.2), and are read again only if demand reaches them. Binary files, files over 1 MiB, and paths that match the exclusions (vendored code, lockfiles, generated code) are not read.

### 8.5 Mechanical extractors

A later stage adds **rules**: patterns kept in the knowledge base that find kinds of mentions without a model, such as the hosts of URLs, the names of queues and topics in configuration, the tables in SQL and the keys of tickets. They run with `git grep -P` at the current revisions. Their matches form an index that the view consults, and they are never stored. A mention becomes a term when a claim, a question or a contribution names it, so the vocabulary grows with use and not with every string in the estate.

## 9. Threads, messages and steps

### 9.1 Where work runs

| Thread | Engine work |
|---|---|
| Client loops | Calls of `knowledge` (searches over the published view, in bounded steps; a large one as a task) and of `contribute` (checks of shape, reading the cited lines with a git child on the call's loop as the `git` tool does, the grounding checks, then a proposal posted to the kb loop); the bundle and the finding of `ask_ai` |
| kb loop (new) | The repository (its writer and readers), the view (building and publishing it), the job queue and the reply cache, the sources' revisions and freshness, demand. It serves no client, as home serves none |
| Home | The engine's jobs, in the background class, as it runs any job |
| Tasks | Building views, validating replies, matching over large texts, in slices of 250 µs on the client loops; the kb loop owns them as client loops own theirs (a small extension: owners have been client loops) |

Home could own the knowledge base, but then building views and validating replies would sit between a job's completion and the next job's start. A sidekick of its own keeps home's turns short.

### 9.2 Messages

- **Client loop to kb loop:** `PROPOSE` (checked objects and the call's handle), `FINDING`, and `DEMAND` (counters, an embedded message with a pending word, so that a burst is one message, core §2).
- **kb loop to client loop:** `VIEW` (a reference) and `DONE` (by the call's handle: the ids, or the reasons for refusing). A handle that has gone stale drops its completion.
- **kb loop and home:** jobs, as between a front and home (`SUBMIT`, `CANCEL`; `DONE`, `PROGRESS`).

A client loop checks a proposal against the view that its call began with. The kb loop merges proposals by identity (§4.2), so two proposals of one term or one claim, checked against different views, become one file. Every exchange ends in exactly one completion. Nothing polls, but for one timer: the sources' refs are read every `--kb-check` seconds (60 by default), because nothing pushes a source's new revision to askai and reading a ref costs little.

### 9.3 Bounds

- Replies and files are parsed in slices, as bodies are (`LOOP_PARSE`, `LOOP_VALUES`).
- Matching counts the bytes it scans under a new unit, `LOOP_MATCH`, with a bound of 64 KiB a step, and PCRE2's match limits bound each match.
- Building a shard of a view counts objects under `LOOP_VALUES`.
- The kb loop runs at most 4 of the 16 children at once, so that interactive `git` and `man` calls keep theirs.

### 9.4 Failure and shutdown

When askai drains, the kb loop stops submitting jobs, lets a commit under way finish (a killed `git fast-import` leaves a lock file, which the notes' check for stale locks removes), cancels its jobs without caching them, and exits after the client loops. If askai is killed, it loses at most the proposals of the current batch, whose calls have not answered yet, so no client was told that they succeeded.

### 9.5 Modules

| Module | Role | Uses |
|---|---|---|
| `gitstore.c` | git plumbing shared with the notes: the fixed environment, argv, input of `fast-import`, parsing of `cat-file` output, rules for names, stale locks | `child.h` |
| `kbobj.c` | objects of the knowledge base: canonical files, ids, merges | json-c, libcrypto |
| `kbterm.c` | normalization of names, matchers of spellings, knitting, matching text | PCRE2 |
| `kbview.c` | views: building, incremental change, digests, references | `kbobj`, `kbterm` |
| `kbquery.c` | `search`, `show` and `related`: ranking and rendering | `kbview` |
| `kbcheck.c` | validation of contributions and replies | `kbobj`, `kbterm` |
| `kbjob.c` | job kinds: recipes, prompts, schemas, windows, request bodies, parsing of replies | `ai.h`, `kbcheck` |
| `kb.c` | the kb loop: writer, publisher, queue, reply cache, sources, freshness, demand | the above, `loop.h`, `job.h`, `child.h`, `task.h` |
| `kbtool.c` | the tools `knowledge` and `contribute`: a built-in plugin with a part on each client loop | `mcp.h`, `kbquery`, `kbcheck` |

`kbobj`, `kbterm`, `kbview`, `kbquery`, `kbcheck` and the parsing in `kbjob` do no I/O and start no threads, so tests link them alone. New dependencies: PCRE2, and a git built with PCRE2 for `git grep -P`, which askai checks for as it starts.

## 10. Validation

Every contribution and every item of a reply passes these checks before it becomes knowledge:

| Check | Rule |
|---|---|
| Shape | The schema, the size limits, a relation of the set, a kind of the set |
| Evidence exists | The revision resolves, the lines exist in the file, and askai copies the quote from git |
| Inside the input | A job's cited lines are within the window or the occurrences that it was shown |
| Grounding | The matchers of the subject, and of an object that is a term, find them in the cited lines, or for a job in its input. A new spelling occurs verbatim in the cited lines (srtview's rule that every `SEEN` spelling is on a cited line). A value is the model's reading of the lines and is not checked, which is why every reader is shown the quote |
| Patterns | They compile under PCRE2 with limits, at most 1 KiB, without backreferences or recursion, their references without cycles; a matcher finds at most 10,000 lines, and a `connect` pattern finds at least one line and at most 2,000 |
| Secrets | A quote that matches a secret rule (keys, tokens, passwords in configuration) is refused. The rules are patterns in the knowledge base |
| Attribution | Testimony comes only from clients, and principal 0 cannot contribute |

srtview founded a new term only on two hits in the corpus. A single hit in a transcript is weak evidence, but a single occurrence of a host name in a configuration file is meaningful, so this floor is not kept. Unspecific matchers are refused instead, and demand gates what is read.

## 11. Security and privacy

- **Exposure.** In the first version every reader of `knowledge` reads the whole knowledge base. This is the same exposure as the `git` tool, whose repositories every caller may read. Before sources with different audiences are listed, roles must grant sources, and the view must filter claims, findings and occurrences by the sources they cite. Every claim records its sources from the start, so this is a filter, not a migration.
- **Injected instructions.** Source text and testimony are untrusted input to the model. Jobs offer no tool but their reply. Replies are bound by schemas and validated. Claims carry their quotes and attribution, so a reader sees where a claim came from, and disputes mark it.
- **Secrets** stay out through the secret rules of §10. The knowledge base copies quotes from the sources, so a secret in a source would otherwise reach everyone who reads the knowledge base.
- **Questions** may carry pasted secrets and personal data. Findings are kept, but by default only their asker is offered them; `--kb-share-findings` shares them with everyone who may read the knowledge base.
- **Attribution.** Contributions record the identity's name, for accountability, as `git blame` does. Findings do not record the asker unless configured.
- **Load.** Matching has limits (§9.3). Search output is paged. `contribute` takes at most 64 objects a minute from one principal.
- **The directory.** askai refuses a knowledge base that others than root and its user may change, as it refuses such a configuration file.

## 12. Proof

- **Unit tests** of the modules without I/O: normalization, matchers, ids, canonical files and merges, building views, ranking and validation, linked without git or a network, as `tests/buf.c` and `tests/handle.c` are.
- **Determinism:** the same contributions in shuffled orders give equal view digests.
- **Git:** checks in `tests/askai.py` with real git, as `check_notes` does: contributions and refusals, an external edit, packing, the lock.
- **The engine end to end** with the fake upstream: canned calls of the reply tool; a cached reply that the mock never sees asked again; identical jobs in flight that reach the mock once; one more attempt with reasons; freshness after a commit to a fixture source; `ask_ai` with a bundle in its prompt, a kept finding served at once, and a finding kept.
- **Load:** a profile with saturating background jobs and contributions beside the probe, judged by the core's verdict (fairness, units of work, outliers), plus one clause: an interactive call waits behind at most one background job on one slot.
- **Quality:** an evaluation set over a fixture estate, questions with the evidence that answers them, measuring the precision of citations, the share of questions answered from the base, and upstream tokens per question. It runs whenever a recipe changes.
- **Metrics** that show whether the cache pays: `ask_ai` calls answered on each level of §6.3, and their turns and upstream tokens; jobs by kind and outcome (cached, done, refused, failed) and their tokens; contributions by type and outcome, refusals by reason; objects by type; claims by freshness; the view's size and build times.

## 13. Build plan

| Stage | Work | Gate |
|---|---|---|
| E0 | `gitstore` out of `notes.c`, used by both; `kbobj`: canonical files, ids, merges | the notes' checks unchanged; unit tests; equal digests in shuffled orders |
| E1 | `kbterm`: normalization, matchers (PCRE2), knitting; `kbview`: built from a tree and published to the loops; `knowledge` over a knowledge base written by hand | searches on a fixture; no step past its bounds; views swapped under load, as `check_reload` does with policies |
| E2 | `contribute`: claims, terms, testimony and verdicts, evidence read from git, grounding; the kb loop as the writer; batched commits | refusals correct and worded; durable on answer; external edits taken in; the lock |
| E3 | `ask_ai` with the base: the bundle, kept findings served at once, lent tools, findings kept, demand recorded | end to end with the fake upstream; the base fills from research |
| E4 | revisions of sources tracked; freshness | fresh, moved, changed and gone on fixture commits; cost in proportion to the changed paths |
| E5 | the job runner, the reply cache, identical jobs in flight as one, required tool calls in each API; `extract` and `describe`; demand ordering | a cached job never reaches the upstream; background never delays interactive (the verdict) |
| E6 | `judge`: nominations, verdicts, entities, merged claims, contradictions in bundles; `recheck` | determinism with verdicts; bounded fan-out |
| E7 | `connect`; rules; sources by role; a saved view, if starts are slow | each its own checks |

E0 to E3 are the smallest useful version: the knowledge base fills from research that is done anyway, and serves it back. Until E4, a bundle says at which commit each claim was read, and nothing about whether it still holds.

## 14. Open decisions and risks

**Open:**

1. **Senses of a name.** One term per normalized name means that a service, its repository and its database are one term, with claims that tell them apart. If that proves confusing, terms can take a qualifier ("orders (table)").
2. **The relations.** Fourteen may be too few, or too many for a small model to choose from reliably. The evaluation set (§12) decides.
3. **Replies.** A required tool call works in every API; a schema for the response (llama-server's `json_schema`, the Responses API's `text.format`, Gemini's `responseSchema`) constrains more where it exists. Start with the tool call.
4. **Sharing findings** by default (§11).
5. **Views past 100,000 claims:** quotes read from git when shown, or a saved view (sqlite, as the core foresaw for its blockers, derived and disposable).
6. **Persistent readers.** A `git cat-file --batch` child per source that stays running, if spawning a child for each read shows up in the measurements.
7. **Embeddings,** as an optional aid for questions that share no word with the base: never part of an identity and never stored. Only if the vocabulary of terms proves too slow to grow.
8. **A model for each kind of job** (a small one for `judge` and `describe`), which needs askai to have more than one upstream.
9. **Summaries of directories** keyed by git tree ids, which is srtview's pyramid with exact invalidation for free; but every commit changes the tree of every directory above it. Only on demand, if at all.

**Risks:**

1. **The model's quality.** Claims may be shallow or wrong. Quotes, disputes, rechecks and the evaluation set catch it; nothing hides it.
2. **Noise in the vocabulary.** Terms are founded by grounded claims, questions and contributions, not by every string; unspecific matchers are refused.
3. **Growth of the repository.** The `replies` branch has its retention (§4.5); claims grow with meaningful change, since a moved claim makes no new file.
4. **Upstream that will not record.** If the upstream rarely calls `contribute`, E3 still keeps findings and demand, and E5's background reading of the files it read covers the rest. The evaluation set measures the rate of recording.
5. **Complexity inside askai's process.** Every engine step runs under the core's rules, and the stutter harness covers it.

## Appendix A. Words

| srtview | askai | Meaning |
|---|---|---|
| topic, `termN`, `focusN` | term | a name, and the matcher that finds it |
| semantic record | claim | an assertion with its evidence |
| concept | merged claim | claims that say the same thing |
| entity | entity | terms that name one thing |
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
| lexicon | entities | joined by verdicts |
| refinery | the kb loop | — |
| lanes, pacing | classes, slots | home's |

**Words of this design:**

| Word | Meaning |
|---|---|
| bundle | what a query returns: terms, claims, findings, occurrences, gaps |
| claim | an assertion relating a term to a term or a value, with evidence |
| demand | decaying counters that order background work |
| entity | terms that verdicts join |
| evidence | lines of a file at a revision, with the quote that askai copied |
| finding | an answer kept with its validated citations |
| freshness | fresh, moved, changed or gone, at the current revisions |
| job | one request of the engine to the upstream |
| kb loop | the sidekick loop that owns the knowledge base |
| matcher | the pattern derived from a term's spellings and written pattern |
| occurrence | a line where a term's matcher matches |
| recipe | the prompt, schema, model and settings of a kind of job |
| source | a git repository that askai lists |
| term | a named thing: name, kind, spellings, pattern |
| testimony | what a person stated, stored as evidence |
| verdict | a judgment about one object or two |
| view | the immutable in-memory index of one commit of the knowledge base |
| window | lines of one file at one blob, which a job reads |
