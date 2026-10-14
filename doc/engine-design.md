# Semantic engine design

Design of askai's semantic engine: the knowledge base that `doc/core-design.md` names as the work the core was built to carry ("the semantic archive, search, prompts and schemas"). It is a first design, for review before any code is written; nothing in it is built. Its ancestor in spirit is srtview's engine (srtview's `DESIGN.md`, `semantic_engine.hpp`, `semantic.cpp`, `refinery.cpp`, `topics.hpp`, `rx.hpp`, `agenda.hpp`, `vault.hpp`); §2 says what carries over and what does not.

## Summary

**The problem.** Consider an organization where people ask how something in a large codebase relates to other code and to the world around it: services, data stores, queues, hosts, partner organizations, teams, documents and procedures. Its systems are large and talk to dozens of peers inside and outside the organization. Its LLM capacity is small, so the usual IDE pattern, an assistant that reads the code afresh for every question, costs too much (§1.2). askai can be the endpoint that these questions pass through. The engine makes each answer leave behind knowledge that later questions can use, also questions worded differently: a semantic cache.

**Decisions**, each argued in its section:

1. **Terms are the currency** (§3.2). A term is a name with the spellings that find it, compiled into a pattern that runs over source text at any revision (`git grep -P`), over questions and over claims. This keeps srtview's regular expressions as the currency of knowledge: a pattern is a search key that can be run again, so it finds evidence again after code moves, and it maps differently worded questions onto the same knowledge without asking a model.
2. **Claims are the knowledge** (§3.3). A claim relates a term to a term or to a value through one relation of a small, fixed set, with a sentence and the exact lines that show it. askai copies the quoted lines from git itself and refuses a claim whose cited lines do not name the things it relates. Words that a model writes are never evidence.
3. **Git holds what cannot be computed again** (§4): terms, claims, verdicts, testimony and answers, in one repository with one writer, as plain files that a person can read, diff and blame, laid out by where writes land so that a commit rewrites a few small trees. Occurrences, freshness and indexes are computed again when needed.
4. **Reads come from an immutable view** (§5): an in-memory index of one commit of the knowledge base, built of persistent maps so that a change copies only what it touches, and published to the client loops by reference, as the policy is. Serving what is known touches neither git nor a model.
5. **Mechanical work first, the client's model second, the upstream last** (§6). The server finds candidates mechanically. The client's own model, which knows the asker's context and runs on the asker's hardware, judges what applies. The upstream is spent on research, and on background reading that never takes the slot kept for interactive work.
6. **Every model job is content-addressed** (§8). A job's id hashes its kind, its recipe and every byte the model is shown. The model reads a window of a file once for all users, until the window or the recipe changes, and identical jobs in flight are one job.
7. **It fits the core** (§9). One new sidekick loop, the kb loop, owns the repository, the view, the job queue and all of the engine's git work. The tools run on the client loops against the published view, model work goes to home as background jobs, and every step is bounded.

**The smallest useful version** (§13, stages E0 to E3) runs no background jobs at all. The knowledge base fills from the research that `ask_ai` calls already do: the upstream records what it learns with `contribute`, askai checks it against the sources, and the next ask starts with it. Background reading, consolidation and freshness come in later stages.

## A question's path

1. Alice's assistant calls `knowledge` with "what sends files to Contoso?". The view matches the term Contoso, a carrier, which is an organization whose spellings are `Contoso` and `CTSO`. No claim is about it yet, so the kb loop looks up its occurrences at the tracked revisions with `git grep`, among them `contoso_sftp_host` at `shipping:config/prod/sftp.yaml:4` and a line of `shipping:src/ship/upload.go:41`, and the result says that nothing is known about what sends to it.
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

- An MCP endpoint with identities, principals and roles. A role grants tools to clients, and its `upstream` member names the tools that the upstream may call in the turns of the role's ask calls.
- `ask_ai` with turns, given `--upstream-tools`: the upstream may call `git`, `man` and the plugins' lent tools as the role's `upstream` member allows, `notes` whenever askai keeps notes, and with those `compact`, `wait` and `timer`, and `background` and `jobs` with a tool that can run in the background.
- The `git` tool: read-only, allowlisted commands in the listed repositories, `grep -P` among them.
- The `notes` tool: private notebooks of the upstream and of clients, each thread a bare git repository, each note a file, written by `git fast-import` and never changed.
- Home's scheduler: upstream slots, deficit round-robin across principals, an interactive and a background class (background jobs hold at most all slots but one; with one slot an interactive job preempts a running background job, which then runs again from its start), one breaker for the upstream, streamed answers and progress.
- Tasks (CPU work in slices of 250 µs), children (pidfd, deadlines, at most 16 at once in the process), blockers, and the stutter harness, whose verdict fails a run on unfairness, unbounded work and gross outliers.
- Plugins that a configuration file loads. A loaded plugin may start no process or thread and cannot reach home, so the engine is built in, as `man` and `git` are.

### 1.4 Goals

- **G1** A question asked again, in the same or other words, costs no research while the knowledge exists and still holds.
- **G2** Everything served points to evidence that a person can check, and says whether that evidence still holds.
- **G3** Interactive work goes first: background jobs never take the slot kept for interactive work, and take at most `--kb-jobs` others. A job is asked of the model again only after a failure, a preemption or a crash.
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
| The projection as a function of the stored facts, independent of the order of completions (`DESIGN.md` I8), checked by a digest (`dirHash`) | Views are reproducible and testable | The view and its digest (§5); time decides nothing (§3.7) |
| Records as triples with a sentence and cited spans; byte-identical twins as one concept; judged pairs (same, related, contradicts, novel); entities as names that verdicts join | It is a workable shape for knowledge between named things | Claims, assertions, verdicts, entities (§3.3, §3.4) |
| "A verdict nobody reads was not worth asking for": contradictions travel with the claims they contradict | Model work is spent only where something reads its result | Bundles (§6.2), nominations (§3.4) |
| A probe proposes a search, the corpus answers it, and the write sees only what the search found | A cheap job produces a mechanical query, and the costly reading is of real evidence | The connect job (§8.1) |
| Recipes: prompt, schema, model and settings in the identity of a cached reply | A changed prompt asks again, and an unchanged one never does | Recipes (§8.2) |
| Bounded fan-out (four judgment candidates, three focus partners), capped retries with feedback | The model's work per new fact is bounded | §3.4, §8.1, §8.2 |
| Retrieval weighted by how rare a word is in the corpus, with no word lists | It works in any language and needs no model | Ranking (§6.2) |
| Heat from user actions steers the background work | Work follows what people ask | Demand (§8.3) |
| "Waiting is a dependency"; workers report and never coordinate; one owner serializes scheduler and projection state (I7) | It keeps concurrency simple | The kb loop (§9) |

### 2.2 What does not carry over

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
| `spellings` | The literal strings it is written as, abbreviations included once evidence shows them: `OrderService`, `ordsvc` |
| `pattern` | Optional: a regular expression that a person wrote, which may refer to other terms as `\{name:}` |
| `by` | Who added what: jobs, models in turns and identities, with times (§3.7) |

**Identity.** A term's identity is its normalized name: the name split into words at changes of case, between letters and digits, and at `_ - . / :` and spaces, case-folded, and joined by single spaces. `OrderService`, `order-service` and `ORDER_SERVICE` are one term, "order service". A name resolves to the term whose normalized name it has, and failing that to the terms that list it as a spelling. Two terms with different names, neither listing the other, can be joined by a verdict into one entity (§3.4): OMS and "order management system". The same name for a service, its repository and its database is one term, and claims tell the facets apart; §14 keeps a split of senses open.

**Matcher.** askai compiles each spelling into a pattern that knows identifiers: the spelling's words case-insensitively, an optional separator between them, and a match that begins and ends at a non-alphanumeric character or at a hump of camel case. So "order service" finds `OrderServiceClient`, `order_service.yaml` and "the order service", but not `reorderService`. In prose (questions and statements), the last word may take a suffix: plurals, and the inflections of languages such as Finnish ("Pythonin" for Python), as srtview's `termMatcher` allowed. A term's matcher is the union of its spellings' patterns, knitted into one pattern (`rx::knit`), and of its written pattern. The spellings are stored, and the pattern is derived from them. srtview stored the knitted pattern and needed `rx::unknit` to open it again; storing the set makes that unnecessary. askai compiles matchers with the PCRE2 options that `git grep -P` uses in askai's fixed environment (`LC_ALL=C.UTF-8`), so that both find the same text.

**Where matchers run:** over source text at a revision with `git grep -P`, which the kb loop runs and which gives the term's **occurrences**; over questions, which gives the terms that a question mentions (§6.1); and over quotes, which grounds claims (§10). In askai's own process, matchers run only over such short texts, under a match limit that bounds each call (§9.3); `git grep` matches the sources in its own processes.

**Limits.** At most 32 spellings, the first 32 in byte order when merged contributions offer more, and a written pattern of at most 1 KiB, without backreferences or recursion, whose references resolve without cycles, as in srtview's `topics.hpp`. A name may be as common as the estate makes it: the name of the organization that owns the estate, or of a core service, occurs everywhere. Ranking weighs such names down by how common they are (§6.2), and listings of occurrences are capped.

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
| `by` | Who asserted it: jobs, models in turns and identities, with times (§3.7) |

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

**Grounding.** A claim cites lines that name its subject and, when its object is a term, its object: the terms' matchers must find them in the quoted lines, which may take two spans when the names stand apart. A value is the model's reading of the lines and is not checked, which is why every reader is shown the quote. Things that the source does not name, such as "the retry mechanism", can live in statements but cannot be subjects. So every claim is anchored to names that the matchers can find again, and the person who reads a quote sees both sides of the claim in it.

### 3.4 Assertions, verdicts, entities and merged claims

An **assertion** is a claim without its evidence: its kind, relation, subject and object. A claim is one observation of an assertion. The same assertion seen in two places, or again at a later revision, is two claims of one assertion.

A **verdict** is a judgment about two assertions or two terms, `same`, `related`, `contradicts` or `unrelated`, or about one assertion, `confirm` or `dispute`, with a reason. Verdicts judge assertions, not claims, so a verdict holds for every observation of what it judges, the later ones included. A verdict has the rank of who gave it: a person, through their client, or a model, as a job or in the turns of an ask call. The view resolves the verdicts on an object from their set alone: verdicts of people outrank those of models; within a rank, verdicts that agree give their value, and verdicts that disagree make the object **contested**, and a contested `same` joins nothing. A person replaces their own verdict on an object by giving another. A pair that has a model's verdict is not judged again.

**Entities.** Terms that a `same` verdict joins form one entity. Its title is the name of its term with the most claims, ties going to the first name in byte order.

**Merged claims.** Assertions with the same kind and relation, the same subject entity and the same object (an entity, or a folded value) are one merged claim, mechanically: srtview's rule that one claim seen in two places is identity, not a judgment. A `same` verdict merges other assertions. A merged claim lists the evidence of all its claims, and its statement is the longest that a person wrote for it, or else the longest of all, ties going to the first in byte order. An assertion that people dispute and no person confirms is left out of bundles (§6.2); `show` and `related` still list it, with its dispute. A claim that contradicts another travels with it in every bundle.

**Nominations.** Pairs are never judged wholesale. A new assertion is judged against at most three assertions that share its subject entity and overlap it most in words. A new term is judged against at most two terms whose names share a word, whose initials spell it (OMS and "order management system"), whose letters it takes in sequence (`ordsvc` and "order service"), or that list it as a spelling. Nominations wait for demand (§8.3): a pair that nobody asks about is not judged.

### 3.5 Findings

A **finding** is an answer that `ask_ai` gave and askai kept: the question and its system text, the digest of the bundle that the upstream was given, the answer, the citations that askai could validate (claims, and spans of sources), the terms that the question matched, the commits of the knowledge base and of the sources it was answered at, the recipe, and the time. Before it is stored, the secret rules (§10) blank what they match in it. A finding belongs to its asker unless the configuration shares findings: it is kept in the asker's private branch (§4.1), which only that principal's calls read. A shared finding is kept in the `knowledge` branch, without its asker. An anonymous asker's findings are kept only where findings are shared, since anonymous clients share one principal. A finding is an event, not a fact. It is offered as an earlier answer, with the freshness of what it cites.

### 3.6 Testimony

**Testimony** is what a person states through their client with `contribute`, at most 8 KiB of text that the secret rules must pass. It is stored as a file in the knowledge base, attributed to the identity, and claims can cite its lines as they cite a source's (§3.1). Testimony is how knowledge that is in no repository gets in: contracts, schedules, contacts, the reasons for decisions. A turn of an ask call cannot testify, because the model has no testimony to give. A client's testimony is attributed to the client's identity, which answers for it whether a person typed it or the person's assistant did.

### 3.7 Identity

| Object | Its identity hashes | Not hashed |
|---|---|---|
| Term | its normalized name | the name as written, kind, spellings, pattern, provenance |
| Assertion | kind, relation, subject, object (a term, or a folded value) | — |
| Claim | its assertion, and its evidence (source, path, blob, lines) | statement, detail, quotes, provenance |
| Verdict | the objects it judges, sorted, and who gave it (an identity's name, or a model's recipe) | value, reason, time |
| Finding | the asker's principal if it is private, the question, system text, answer, citations and time | — |
| Testimony | identity, text, time | — |
| Job | kind, recipe, every byte the model is shown | — |

An id is the first 128 bits of SHA-256, over a tag naming the type and the canonical fields, written as 32 hexadecimal digits. The tools take any unique prefix of at least 8 digits.

**Time decides nothing.** Contributions carry the time they were made, for people to read. No rule of the view, and no rule that merges a file, chooses by time, so the knowledge base and its view depend only on what was contributed, never on the order in which it arrived, but for the recorded times themselves (srtview's I8). Findings and testimony are events, so their time is part of what they are and of their identity. A person's later verdict replaces their earlier one because the person replaced it. A claim stays true of the commit that it cites; the same assertion at a new revision is a new claim of the same assertion.

### 3.8 Freshness

At a source's current revision, each span of evidence is:

- **fresh**: its blob is unchanged, or its quote is still at its lines;
- **moved**: its quote is found verbatim at other lines of the same file, or in the file that `git diff-tree` reports it was renamed to;
- **changed**: neither, but the claim's terms are still found in the file;
- **gone**: none of these.

A quote names its claim's terms (§3.3), so a quote found again is the same text, not a stray `}`. A claim's freshness is that of its best span. A moved claim keeps its file: the view records where its quote is now, and nothing is asked or written. When a tracked ref moves, the kb loop asks `git diff-tree` for the paths that changed, and checks only the spans in those paths, each with a read of the new blob and a search of its bytes. No model is asked. Freshness is not stored: it is a function of the knowledge base, the sources' revisions and the outcomes of rechecks, and cheap to compute again. A changed claim is a candidate for a recheck job (§8.1) when there is demand for it, and a recheck that finds no support makes it count as gone. A gone claim is history: still true of its commit, and offered last.

## 4. The knowledge base in git

### 4.1 Layout

```text
KB.git                         bare; --kb DIR, made if it does not exist
  refs/heads/knowledge
    FORMAT                     the version of the format, and its relations
    terms/or/order-service.json
                               one file per term, by its normalized name
    claims/sources/shipping/src/ship/upload.go/5d1e...07.json
                               one file per claim, under the file that its
                               first span cites
    claims/testimony/alice/7e21...5a/c0ff...ee.json
                               the claims that cite one testimony
    verdicts/a0/a0c4...9b/alice.json
                               one file per judged object and judge
    testimony/alice/2026-10-14/7e21...5a.md
                               one file per statement, as given
    findings/2026-10-14/0c4d...11.json
                               shared findings
  refs/heads/private/<principal>
    findings/2026-10-14/...    an asker's own findings
  refs/heads/replies
    extract/2026-10-14/4c1f...08.json
                               each job's reply and outcome
    recipes/91be...2d.txt      each recipe's prompt, schema and settings
```

Paths follow where writes land. The claims of one `extract` job cite one file, so its commit touches that file's directory and the few above it; testimony, findings and replies land in the day's directory. A commit thus rewrites a few small trees. A fan-out by id would scatter every batch over most directories and rewrite most of the tree, at a cost that grows with the knowledge base. Measured with `git fast-import` from the branch's tip (git 2.43, 4 CPUs; claim files of about 700 bytes, in a synthetic estate of 20 sources with 1,000 directories and 10,000 files each; scratch work, not in the repository):

| Commit | 100,000 files, by id | 100,000 files, this layout | 1,000,000 files, by id | 1,000,000 files, this layout |
|---|---|---|---|---|
| 1 new file | 13–21 ms, +24–26 KB | 10–12 ms, +2.2–2.3 KB | 32–37 ms, +178–188 KB | 10–13 ms, +2.5–2.8 KB |
| 256 claims of one source file | 256–306 ms, +3.0–3.1 MB | 33–44 ms, +131 KB | 6.5–7.4 s, +28–29 MB | 29–38 ms, +131 KB |
| 256 claims of 256 source files | 277–323 ms, +2.9–3.2 MB | 103–117 ms, +310 KB | 5.8–7.7 s, +27–30 MB | 100–156 ms, +404 KB |

Under this layout a commit costs about what its own files do, whatever the size of the knowledge base. The layout also reads well: what the knowledge base says about a file is found under that file's path. A term's file is named by its normalized name, its spaces made hyphens, under its first two letters; a name that cannot be a file name is named by its id.

### 4.2 Files

Each file is canonical JSON with its members in a fixed order, one member per line, so that a diff reads as a change of fields. A file's content is a function of the set of contributions to its object, by rules that read only the values: provenance and spellings are sets, kept sorted and capped (one entry of provenance for each contributor, an identity's name or a job's id, at most 16, people's first by name and then models' by id; 32 spellings in byte order), and a single field takes a person's value over a model's, then the longest statement or the first other value in byte order. The writer rewrites a file when a contribution adds to it, and the order in which contributions arrived never shows in the tree (§3.7).

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
{"identity": "alice", "at": "2026-10-14T13:05:40Z"},
{"job": "4c1f...", "recipe": "91be...", "at": "2026-10-14T13:02:11Z"}
]
}
```

### 4.3 One writer

The kb loop is the only writer. It gathers accepted contributions for at most 50 ms, or until it has 256 objects, and writes them with one run of `git fast-import` from the tips of the branches: on `knowledge`, one commit for each identity that contributed, with that identity as its author, and one for the models' contributions, with askai as its author; on the other branches, the replies of the jobs whose results the batch holds and the private findings. Each commit's message lists the ids of what it added or confirmed, also when a capped file did not change, so `git log --grep` finds every contribution.

If a tip has moved, an external edit came in between: the kb loop takes that edit in (§4.5) and writes the batch again. If the write fails otherwise (a full disk, a lock, an error of git's), the calls waiting on the batch answer with the failure, so no caller is told that a write succeeded when it did not; the jobs' results stay in memory and are written again after a backoff. While it runs, the kb loop holds an advisory lock (`flock`) on the directory, which the kernel releases if askai dies, so that a second askai cannot write the same knowledge base. Holding it, the kb loop can remove at start the lock files that a killed git left behind, since no other writer can own them.

### 4.4 What is not stored

Occurrences, freshness, indexes, scores and the mentions that rules find (§8.5) are not stored: they can all be computed again from the knowledge base and the sources, and storing them would make the repository churn with every commit of every source. Demand (§8.3) is not stored either. It is advisory, it is lost when askai stops, and it builds up again with use.

### 4.5 Operations

- **Packing.** Small imports become loose objects (`fastimport.unpackLimit`) and larger ones small packs. After every 256 commits the kb loop runs `git repack --geometric=2 -d`, which rolls the small packs into fewer and costs little when it runs often; no batch is written while it runs, so no write races it. A whole-repository `git gc` runs only when an operator asks, and the kb loop holds its writes meanwhile.
- **Retention.** The `replies` branch is a cache. Pruning it by age drops the texts of old replies but keeps each job's id and outcome, so pruning never makes the engine ask again.
- **Replication.** The repository can be cloned and fetched, its private branches only if the operator includes them. A view builds from any commit, so a read-only askai can serve `knowledge` from a mirror.
- **External edits.** A curator may edit the `knowledge` branch directly, for example to fix a term's kind or the way its name is written, but not its normalized name, which is its identity: a real rename is a new term and a `same` verdict. When the kb loop finds a tip that it did not write, it validates the changed files as it validates contributions (§10), applies the valid ones to the view, and logs the invalid ones, which every view built later skips in the same way. A person withdraws a claim by disputing its assertion (§3.4). Deleting the file would not do, because a cached reply would add it again.
- **Audit.** `knowledge show` gives an object's history: the commits that added or confirmed it, and its verdicts. `git log` of its file, and `git log --grep` with its id, give the same from git.

## 5. The view

The **view** is an immutable, in-memory index of one commit of the `knowledge` branch, with a layer of freshness for the sources' current revisions. It holds:

- terms by id, and an index from a short prefix of each spelling's first word to the terms, for matching questions;
- compiled matchers, for the terms that have written patterns;
- entities and merged claims, as maps from each member to its group;
- claims and assertions by id, and postings from each entity to its assertions, by relation and direction;
- verdicts by object, and findings by term;
- word statistics (how many statements, questions and names each word occurs in) for ranking;
- the freshness of each claim at the current revisions.

**Persistent maps.** The maps are hash tries, as Clojure's maps are: a change copies the nodes on its path, a handful of them (the logarithm, base 32, of the map's size), and the new view shares every other node with the old one. A batch of 256 objects copies some hundreds of small nodes, not the view. Postings grow by appending. Groups grow by relabeling the smaller group of a union, and a verdict that splits a group (a person's `unrelated` over a model's `same`) recomputes that group alone.

**Changes.** The kb loop applies a commit's objects to a new view in bounded steps on its own thread, and publishes the view by posting a reference to every client loop, at most one view every 100 ms. A loop takes the new view for the calls that begin from then on and lets go of the old one, as it does with a reloaded policy (core §6, R5). A call holds a view only while its steps use it: a `knowledge` call for its milliseconds, and an `ask_ai` call while it builds its bundle and again while it checks its citations, never across its turns. An old view thus lives for milliseconds after its successor, and keeps alive only the nodes that its successor replaced.

**A caller's own writes.** A `contribute` call answers only after the view that holds its write has been posted to the call's loop, and a loop handles each producer's messages in order (core §2), so the next call on that loop sees the write.

**Determinism.** A view's digest is a hash over its canonical dump, recorded times left out, and depends only on the tree of the `knowledge` branch and the version of the format. A test commits the same contributions in shuffled orders, with their recorded times shuffled too, and compares the digests. This is srtview's `dirHash`, made a gate.

**Size.** A claim takes about 1 KiB in the view, so 100,000 claims take about 100 MB. Beyond that, quotes and statements can stay in git and be read when shown, or a saved view can speed up starts, but never as the authority (§14).

**Start.** The kb loop builds the first view from the branch's tip a subtree at a time: `git ls-tree` of a subtree, then `git cat-file --batch` of its files, each a child whose output is capped (16 MiB) and parsed in bounded steps, so memory stays bounded whatever the size of the knowledge base. The `replies` branch is indexed from its file names alone, which carry each job's id, day and outcome. Until the first view is published, `knowledge` answers that the knowledge base is loading.

## 6. Retrieval and the semantic cache

### 6.1 From words to terms

The text of a query is split into words and folded, identifiers included, so `OrderService` in a question becomes "order service". The first four characters of each word, or the whole word if it is shorter, look up the terms whose spellings begin with a word that starts the same way, and those terms' matchers, in their prose form, confirm or reject each candidate; the matchers of terms with written patterns run over the text as well. Matched terms expand to their entities. An identifier in the question that no term matches (camel case, snake case, a path) is looked up in the sources by the kb loop as a literal: a hit is offered as a candidate term, which the client can add with `contribute`.

### 6.2 Bundles

A **bundle** is what a query returns:

- **Terms** matched, with their kinds, entities, spellings and definitions.
- **Claims**, ranked: claims that link two of the matched terms first, since they answer relationship questions; then by the rarity of their shared terms and the overlap of their statements with the query, both weighted by how rare each word is (srtview's `vocab`); fresh claims before changed ones, and gone ones last; confirmed claims up, contested ones marked, and assertions that people dispute and no person confirms left out. A claim that contradicts a ranked claim comes with it. Words are compared by their first six characters, so "shipments" meets "shipment" and "Pythonin" meets "Python". srtview compared whole words; the cut needs no word list either, and it tolerates plurals and the inflections of languages such as Finnish.
- **Findings** that the asker may read (their own, and shared ones) whose terms and questions overlap the query, with the freshness of what they cite.
- **Occurrences**: lines where matched terms occur that no claim explains. The kb loop looks them up with `git grep -P` at the current revisions and keeps them by matcher and revision. A search waits for them at most 2 s, then answers with what it has and says what is still being looked up. They are raw material that the client's model can read itself.
- **Gaps**: matched terms that have no claims, and pairs of matched terms that no claim connects.

Output is text, paged with `offset` as the `git` and `notes` tools are, and in a turn within a window of an eighth of the context.

### 6.3 Three levels of reuse

1. **The same question over the same knowledge.** An `ask_ai` call whose normalized prompt, system text, recipe and bundle digest equal those of a finding that the asker may read is answered with that finding at once, marked as such, if what the finding rests on still holds: before freshness exists (E4), if the sources are still at the revisions that the finding recorded; after, if its citations are fresh. A new answer could be worded differently, but it would rest on the same knowledge; this is srtview's identity of an answer. A call that gives `fresh: true` skips this level, and so does a call that goes on with a notes thread.
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
| `related` | `name`, optional `relation`, `depth` of at most 3 | the graph around a term, for impact questions: paths of claims, at most 16 edges a hop and 64 in all |

### 7.2 `contribute`

Writes, lent to turns when the role's `upstream` member grants it. A client's contributions have its identity's rank, a person's; the upstream's in turns have a model's (§3.4), and the identity behind them is in askai's log, not in the knowledge base.

| Action | Arguments | What askai does |
|---|---|---|
| `claim` | `subject`, `relation`, `object` or `value`, `statement`, optional `detail` and `kind`, `evidence`: `[{source, path, lines, rev}]`, or `{testimony, lines}` for testimony | posts the claim to the kb loop, which reads the cited lines at `rev` (by default the source's tracked ref, as it is when askai reads them), copies the quote, checks the claim (§10), writes it, and answers with the claim's id and the terms that it found or made, or with every reason for refusing it, worded so that a model can correct the claim and try again |
| `term` | `name`, `kind`, `spellings`, `evidence`, optional `pattern` | checks that each new spelling occurs in the cited lines, checks the pattern, and adds or extends the term |
| `testimony` | `text`, optional `about`: names | stores the text, attributed (§3.6), and answers with its id and the address that claims cite it by; clients only, never turns |
| `verdict` | `ids` (one or two), `value`, `reason` | records the judgment, at the rank of who gave it |

A write is durable and visible when its call answers: the reply waits for its batch's commit (§4.3) and for its view (§5), some tens to a few hundred milliseconds.

### 7.3 In `ask_ai`

With `--kb` and `--upstream-tools`:

- Before the first turn, askai runs a search over the prompt. If a finding matches on level 1 (§6.3), askai answers with it at once. Otherwise it adds the bundle to the prompt in brackets, as it adds notes, within an eighth of the context and `--kb-bundle` (16 KiB by default).
- Turns get `knowledge` and `contribute` as the role's `upstream` member grants them.
- After the answer, askai checks the citations in it (`[c:5d1e9b20]` for a claim, `[shipping:src/ship/upload.go:38-47]` for lines): claims against the view, and lines by having the kb loop read them. It keeps a finding with the valid ones. The bundle shows every claim in the form that cites it, and `contribute` answers with that form, so the upstream sees it before it answers. The trace that the client receives says what was recorded.
- The files that the turns read with `git` add to demand (§8.3).

A configuration in which clients read the knowledge base, the upstream writes to it with evidence that askai checks, and curators write as people (the `upstream` member landed in 9bd5f42):

```json
{
  "kb": "/var/lib/askai/kb.git",
  "upstream-tools": true,
  "roles": {
    "user":    { "tools": ["ask_ai", "knowledge"],
                 "upstream": ["git", "knowledge", "contribute"] },
    "curator": { "tools": ["ask_ai", "git", "knowledge", "contribute"] }
  }
}
```

### 7.4 Notes stay separate

The `notes` tool stays as it is: private, per thread, unvalidated, the upstream's working memory. The knowledge base is shared, validated and attributed. Merging the two would either weaken the knowledge base's checks or burden the notebook with them. They share plumbing instead. What the notes need of git, now split between `notes.c` (the fixed environment, argv, the input of `git fast-import`, the parsing of `git cat-file`'s output, the rules for names) and `mcp.c` (spawning git with deadlines, removing stale locks), becomes a module that both use (§13, E0).

## 8. Background jobs

### 8.1 Kinds

| Kind | The model is shown | It answers | Queued when |
|---|---|---|---|
| `extract` | a window of a file (source, path, numbered lines, §8.4), or an excerpt | terms with spellings, and claims citing lines that it was shown | there is demand on its file, or the file is a boundary file (§8.3) |
| `describe` | a term and an excerpt of up to 24 KiB of its occurrences: lines from several files, each block under its source and path | a definition and other claims citing those lines | a term that someone asked about has no definition |
| `judge` | two assertions with their quotes, or two terms with their claims or occurrences | a verdict and its reason | a nominated pair (§3.4) has demand |
| `connect` | two terms and up to 8 KiB of the occurrences of each | a pattern to search for code that involves both, or none | a question relates two terms that no claim connects |
| `recheck` | a changed claim and the current text where its evidence was | whether the text still supports it, and the lines that do | a changed claim (§3.8) has demand |

`connect` is srtview's probe, search and focus as one chain. Its pattern is validated (§10) and run with `git grep -P` at the current revisions, and up to 24 KiB of the lines it found, with their neighbors, become the excerpt of one `extract`, which the reply cache serves like any other. One `connect` thus costs at most two jobs, as srtview's probe and focus did. A `recheck` that finds support yields a new claim of the same assertion with new evidence. One that does not makes the old claim count as gone (§3.8), and the old claim stays true of its commit.

There is no answer kind, because `ask_ai` is how answers are made. There are no summaries by default (§14).

### 8.2 One runner

- **Identity.** A job's id is a hash of its kind, its recipe and every byte that the model is shown, and of nothing else. srtview kept a video's title out of the identity because a renamed video is the same video. Here the path is shown and hashed, because a configuration file under `prod/` and the same file under `staging/` say different things.
- **Reply cache.** The `replies` branch records each job's reply and outcome. A job whose reply is cached is not asked again. If demand reaches a cached job whose claims the knowledge base lacks (a knowledge base rebuilt, or the cache carried to another deployment), its cached reply is validated again, without an upstream call. A job in flight with the same id takes the new demand, and nothing is submitted twice.
- **Submission.** Jobs go to home as a front's jobs do, in the background class, at most `--kb-jobs` at a time (2 by default; 0 runs none). Home refuses a job at once while its breaker is open, or when too many jobs are queued; the kb loop then submits nothing until the time that the refusal names has passed, or for a backoff that doubles from 1 s to 60 s, so it neither spins nor polls.
- **The breaker.** Home's one breaker counts every job's failures, so five failures of background jobs in a row would refuse interactive calls for up to a minute. The engine needs a small change to home: background jobs' outcomes do not count toward the breaker. If the upstream is down, interactive calls open the breaker on their own, and the kb loop backs off on its jobs' failures.
- **Replies.** The request offers one tool, whose input schema is the kind's reply, and requires the model to call a tool. Each of the four APIs can require a tool call, and with one tool offered that call is the reply. askai already reads the tool calls of each API from a stream (`ai.c`). Anthropic's API refuses a required tool call while extended thinking is on, so there engine jobs run without thinking, or offer the tool unrequired and count an answer without the call as an invalid reply. Where a server can constrain the output to the schema (llama-server's grammars), that is an optimization, and askai validates every reply all the same (§10).
- **Outcomes.** askai keeps the valid items of a reply and counts the invalid ones, so that one invalid item does not lose its valid siblings (srtview's rule). A reply that is invalid as a whole gets one more attempt, under a new id, with the validator's reasons appended; a second invalid reply is recorded in the cache as such. A job that the upstream fails twice (a timeout, a stall, an error) is recorded as failed. Either way, the job is not asked again under this recipe, but for a failed job when demand reaches it again after 7 days, as an outage of the upstream is no answer.
- **Recipes.** A recipe hashes the kind, the prompt, the schema, the model as the upstream names it (llama-server's `/props`, a provider's model, or `--kb-model` for a server that names none), the token limit, the temperature and `--extra`. Its text is stored in the `replies` branch for audit. A new recipe misses the cache. It does not touch the knowledge.

### 8.3 What runs next

The queue holds what to do, not the inputs: a job's kind and what it is about (a file and its window, a term, a pair). It holds at most 4,096 entries, and when it is full the least demanded go. The kb loop gathers a job's inputs (the window's text, the occurrences, the samples) when the job is about to be submitted.

The kb loop orders the queue by **demand**: counters on terms and files that decay with a half-life of 7 days. Searches feed them (the terms they matched and the files of those terms' occurrences), and so do research (the files that the turns read), gaps (matched terms without claims) and disputes. Each kind of file has a weight, and boundary files come first: configuration, interface definitions, deployment manifests and documentation, where code meets the world. Boundary files that no demand has reached enter the queue as it has room, a source at a time, so that idle capacity reads them. Ties go to the smaller id. A `describe` or `connect` about the same term or pair is not asked again within 30 days, whatever it answered, so that a "none" is not asked again every time demand returns.

### 8.4 Windows

A window is a run of lines of one file at one blob, at most 12 KiB. Its cut points are chosen by content, as backup tools chunk files: every line may end a window, and a line ends one when a hash of it and the two lines before it meets a condition, at least 2 KiB after the last cut; a window that reaches 12 KiB is cut by force. A forced cut shifts only the window that it ends, because the next cut that content chooses falls where it would have fallen; so an edit re-keys only the windows near it, whatever the indentation of the code. Each window repeats the last five lines of the one before it. Its lines are numbered from 1 within the window, and askai adds the window's offset to the lines that the model cites, so a window whose text is unchanged has the same id wherever the file puts it. A window that only moved within its file therefore keeps its id and is never read again (§8.2), and its claims are moved, not added. A renamed file's windows have new ids, because the path is part of them, and are read again only if demand reaches them. Binary files, files over 1 MiB, files whose lines are too long to cut (minified code) and paths that match the exclusions (vendored code, lockfiles, generated code) are not read.

### 8.5 Mechanical extractors

A later stage adds **rules**: patterns kept in the knowledge base that find kinds of mentions without a model, such as the hosts of URLs, the names of queues and topics in configuration, the tables in SQL and the keys of tickets. They run with `git grep -P` at the current revisions. Their matches form an index that the view consults, and they are never stored. A mention becomes a term when a claim, a question or a contribution names it, so the vocabulary grows with use and not with every string in the estate.

## 9. Threads, messages and steps

### 9.1 Where work runs

| Thread | Engine work |
|---|---|
| Client loops | Calls of `knowledge`: matching and ranking over the published view, in bounded steps. Calls of `contribute`: checks of shape, then the contribution posted to the kb loop. The bundle and the finding of `ask_ai`. No git: what needs git, a client loop asks of the kb loop |
| kb loop (new) | All of the engine's git work, within its own budget of children (§9.3): the reads that calls wait for (cited lines, occurrences, literals) before those of freshness, jobs and the start; validation and grounding of contributions and replies; the writer; the view, built and published; the job queue and the reply cache; demand. It serves no client, as home serves none |
| Home | The engine's jobs, in the background class, as it runs any job, its breaker leaving them out (§8.2) |

Home could own the knowledge base, but then building views and validating replies would sit between a job's completion and the next job's start. A sidekick of its own keeps home's turns short. The kb loop orders its own steps as any loop does, the work that a call waits for before the background work.

### 9.2 Messages

- **Client loop to kb loop:** `PROPOSE` (a call's contributions, checked for shape, and the call's handle), `READ` (spans of lines that a call needs, such as an answer's citations), `OCCUR` (occurrences and literals that a search needs), `FINDING`, and `DEMAND`.
- **kb loop to client loop:** `VIEW` (a reference), and `DONE` (by the call's handle: the ids, the lines, the occurrences, or the reasons for refusing). A handle that has gone stale drops its completion.
- **kb loop and home:** jobs, as between a front and home (`SUBMIT`, `CANCEL`; `DONE`, `PROGRESS`).

Calls that wait for the same lookup share it, as calls for one manual page share its rendering. Demand travels in a buffer that the client loop fills and hands over whole; the kb loop merges it and hands the emptied buffer back, while the client loop fills a second one. Each client loop has its two buffers from its start, so a post needs no allocation, and nothing is shared between the threads (R1, R3). A buffer that fills before it can be handed over drops what does not fit, as demand is advisory.

The kb loop checks a contribution against the view that it holds when it takes the contribution, and merges contributions by identity (§4.2), so two contributions of one term or one claim, from different calls, become one file. Every exchange ends in exactly one completion. Nothing polls, but for one timer: the sources' refs are read every `--kb-check` seconds (60 by default), because nothing pushes a source's new revision to askai and reading a ref costs little.

### 9.3 Bounds

- Replies and files are parsed in slices, as bodies are (`LOOP_PARSE`, `LOOP_VALUES`).
- In askai's process, matchers run only over short texts: questions of at most 8 KiB and quotes of at most 4 KiB. PCRE2's match limit is set so that one call stays within a step's budget. A call that reaches the limit counts as no match and is logged, and a written pattern that reaches it while it is validated is refused. A new unit, `LOOP_MATCH`, counts the bytes that a step matches, with a bound of 64 KiB. Matching over the sources is `git grep`'s, in its own processes.
- Applying a commit to a view counts objects under `LOOP_VALUES`.
- The kb loop runs at most 4 of the process's 16 children at once (`--kb-children`), so that clients' `git`, `man` and `notes` calls keep theirs. Reads of many spans of one source share one `git cat-file --batch`.

### 9.4 Life cycle

The kb loop is a loop as home is, with its own number in the handles: with the engine on, client loops number at most 254. The brainstem starts it after home and before the client loops, which answer `knowledge` as loading until its first view comes. When askai drains, the client loops drain first, their calls ending as in any drain; then the kb loop writes the batch it holds, cancels its jobs without caching them and exits; then home. A contribution written after its call ended does no harm: a retry of it merges into the same file. In a relief of memory pressure, the kb loop gives back its cached occurrences and its kept buffers, as a client loop gives back its kept requests. If askai is killed, it loses at most the contributions of the current batch, whose calls had not answered, so no client was told that they succeeded; jobs whose replies were not yet written are asked again.

### 9.5 Modules

| Module | Role | Uses |
|---|---|---|
| `gitstore.c` | git plumbing shared with the notes: the fixed environment, argv, input of `fast-import`, parsing of `cat-file` output, rules for names, spawning with deadlines, stale locks | `child.h` |
| `kbobj.c` | objects of the knowledge base: canonical files, ids, merges | json-c, libcrypto |
| `kbterm.c` | normalization of names, matchers of spellings, knitting, matching text | PCRE2 |
| `kbview.c` | views: persistent maps, changes, digests, references | `kbobj`, `kbterm` |
| `kbquery.c` | `search`, `show` and `related`: ranking and rendering | `kbview` |
| `kbcheck.c` | validation and grounding of contributions and replies | `kbobj`, `kbterm` |
| `kbjob.c` | job kinds: recipes, prompts, schemas, windows, excerpts, request bodies, parsing of replies | `ai.h`, `kbcheck` |
| `kb.c` | the kb loop: git work, writer, views, queue, reply cache, freshness, demand | the above, `loop.h`, `job.h`, `child.h` |
| `kbtool.c` | the tools `knowledge` and `contribute`: a built-in plugin with a part on each client loop | `mcp.h`, `kbquery` |

`kbobj`, `kbterm`, `kbview`, `kbquery`, `kbcheck` and the parsing in `kbjob` do no I/O and start no threads, so tests link them alone. New dependencies: PCRE2, and a git built with PCRE2 for `git grep -P`, which askai checks for as it starts.

## 10. Validation

Every contribution and every item of a reply passes these checks before it becomes knowledge:

| Check | Rule |
|---|---|
| Shape | The schema, the size limits, a relation of the set, a kind of the set |
| Evidence exists | The revision resolves, the lines exist in the file, and askai copies the quote from git |
| Inside the input | A job's cited lines are within the window or the excerpt that it was shown |
| Grounding | The cited lines name the subject and, when the object is a term, the object: their matchers find them in the quotes (§3.3). Each new spelling occurs verbatim in the cited lines (srtview's rule that every `SEEN` spelling is on a cited line). A value is not checked |
| Patterns | A written pattern compiles under PCRE2, is at most 1 KiB, has no backreferences or recursion, its references resolve without cycles, and it stays within the match limit on its own evidence. A `connect` pattern finds at least one line and at most 2,000 |
| Secrets | Text that matches a secret rule (keys, tokens, passwords in configuration) is refused in quotes, statements and testimony, and blanked in findings. The rules are patterns in the knowledge base |
| Rank | Testimony comes only from clients. A client's contributions have its identity's rank, and the upstream's in turns a model's. Principal 0 cannot contribute |

srtview founded a new term only on two hits in the corpus. A single hit in a transcript is weak evidence, but a single occurrence of a host name in a configuration file is meaningful, so this floor is not kept; grounding does its work.

## 11. Security and privacy

- **Exposure.** Every reader of `knowledge` reads what the knowledge base learned from every source. A role whose upstream may call `git` can learn as much through `ask_ai`'s answers, but a role that is granted `knowledge` and not `git` gains read access to every source, in digested form. Until roles grant sources, grant `knowledge` only to roles that may read every listed source. Then the view will filter claims, findings and occurrences by the sources they cite. Every claim records its sources from the start, so that will be a filter, not a migration.
- **Injected instructions.** Source text and testimony are untrusted input to the model. Jobs offer no tool but their reply. Replies are bound by schemas and validated. Claims carry their quotes and attribution, so a reader sees where a claim came from, and disputes mark it. Contributions from the upstream in turns have a model's rank, so text that steers the upstream cannot outrank people.
- **Secrets.** The secret rules of §10 apply to everything stored: quotes, statements and testimony are refused, and findings blanked. The knowledge base copies quotes from the sources, so without the rules a secret in a source would reach everyone who reads the knowledge base.
- **Questions** may carry pasted secrets and personal data. A finding belongs to its asker, in a private branch that only that principal's calls read (§3.5), unless `--kb-share-findings` shares findings, without their askers.
- **Attribution.** A client's contributions record its identity's name, for accountability, as `git blame` does. The upstream's contributions in turns, and shared findings, record no identity in the knowledge base; askai's log has who asked.
- **Load.** Matching has limits (§9.3), `related` has its caps, and search output is paged. `contribute` takes at most 64 objects a minute from one principal.
- **The directory.** askai refuses a knowledge base that others than root and its user may change, as it refuses such a configuration file.

## 12. Proof

- **Unit tests** of the modules without I/O: normalization, matchers, ids, canonical files and merges, persistent maps and views, ranking and validation, linked without git or a network, as `tests/buf.c` and `tests/handle.c` are.
- **Determinism:** the same contributions in shuffled orders, with their recorded times shuffled too, give equal view digests.
- **Git:** checks in `tests/askai.py` with real git, as `check_notes` does: contributions and refusals, an external edit, a failed write, packing, the lock.
- **The engine end to end** with the fake upstream: canned calls of the reply tool; a cached reply that the mock never sees asked again; identical jobs in flight that reach the mock once; one more attempt with reasons; refusals by the breaker and by a full queue, answered with a backoff and no storm of submissions; freshness after a commit to a fixture source; `ask_ai` with a bundle in its prompt, a finding served at once on level 1, and a finding kept.
- **Commit costs:** the measurements of §4.1, repeated in E0 at the size of a large estate, set the batch window.
- **Load:** a profile with saturating background jobs and contributions beside the probe, judged by the core's verdict (fairness, units of work, outliers), plus one clause: an interactive call waits behind at most one background job on one slot.
- **Quality:** an evaluation set over a fixture estate, questions with the evidence that answers them, measuring the precision of citations, the share of questions answered from the base, and upstream tokens per question. It runs whenever a recipe changes.
- **Metrics** that show whether the cache pays: `ask_ai` calls answered on each level of §6.3, and their turns and upstream tokens; jobs by kind and outcome (cached, done, refused, failed) and their tokens; contributions by type and outcome, refusals by reason; objects by type; claims by freshness; the view's size and build times; commits and their costs.

## 13. Build plan

| Stage | Work | Gate |
|---|---|---|
| E0 | `gitstore` out of `notes.c` and `mcp.c`, used by both; `kbobj`: canonical files, ids, merges; the layout's commit costs at scale | the notes' checks unchanged; unit tests; equal digests in shuffled orders, times included; commit costs measured |
| E1 | `kbterm`: normalization, matchers (PCRE2, with git's options), knitting; `kbview`: persistent maps, built a subtree at a time and published to the loops; the kb loop's number and life cycle; `knowledge` over a knowledge base written by hand | searches on a fixture; matchers and `git grep -P` agree beyond ASCII; no step past its bounds; views swapped under load, as `check_reload` does with policies |
| E2 | `contribute`: claims, terms, testimony and verdicts, lines read by the kb loop, grounding, ranks; the writer, with batched commits by author; demand buffers | refusals correct and worded; writes durable and visible on answer; external edits taken in; failed writes answered; the lock |
| E3 | `ask_ai` with the base: the bundle, findings kept (private, or shared), level 1 by equal revisions, lent tools, citations checked | end to end with the fake upstream; the base fills from research |
| E4 | revisions of sources tracked; freshness; level 1 by fresh citations | fresh, moved, changed and gone on fixture commits; cost in proportion to the changed paths |
| E5 | the job runner: reply cache, jobs in flight shared, backoff on refusals, home's breaker leaving background jobs out, required tool calls in each API; `extract` and `describe` over content-chosen windows and excerpts; demand ordering | a cached job never reaches the upstream; background work never delays interactive work beyond G3 (the verdict); an edit re-keys only the windows near it |
| E6 | `judge`: nominations, verdicts, entities, merged claims, contested and disputed assertions, contradictions in bundles; `recheck` | determinism with verdicts; bounded fan-out |
| E7 | `connect`; rules; sources by role; a saved view, if starts are slow | each its own checks |

E0 to E3 are the smallest useful version: the knowledge base fills from research that is done anyway, and serves it back. Until E4, a bundle says at which commit each claim was read, and nothing about whether it still holds.

## 14. Open decisions and risks

**Open:**

1. **Senses of a name.** One term per normalized name means that a service, its repository and its database are one term, with claims that tell them apart. If that proves confusing, terms can take a qualifier ("orders (table)").
2. **The relations.** Fourteen may be too few, or too many for a small model to choose from reliably. The evaluation set (§12) decides.
3. **Replies.** A required tool call works in every API but Anthropic's with extended thinking; a schema for the response (llama-server's `json_schema`, the Responses API's `text.format`, Gemini's `responseSchema`) constrains more where it exists. Start with the tool call.
4. **Sharing findings** by default (§11).
5. **Views past 100,000 claims:** quotes read from git when shown, or a saved view (sqlite, as the core foresaw for its blockers, derived and disposable).
6. **Persistent readers.** A `git cat-file --batch` child per source that stays running, if spawning a child for each read shows up in the measurements.
7. **A child that streams.** `child.h` hands a child's output over once all of it has arrived; the start and the lookups of occurrences would hold less memory if the output reached its owner in steps.
8. **Embeddings,** as an optional aid for questions that share no word with the base: never part of an identity and never stored. Only if the vocabulary of terms proves too slow to grow.
9. **A model for each kind of job** (a small one for `judge` and `describe`), which needs askai to have more than one upstream.
10. **Summaries of directories** keyed by git tree ids, which is srtview's pyramid with exact invalidation for free; but every commit changes the tree of every directory above it. Only on demand, if at all.

**Risks:**

1. **The model's quality.** Claims may be shallow or wrong. Quotes, disputes, rechecks and the evaluation set catch it; nothing hides it.
2. **Noise in the vocabulary.** Terms come from grounded claims, questions and contributions, not from every string, and common names weigh little in ranking.
3. **Growth of the repository.** A commit rewrites only the trees along its paths (§4.1), the `replies` branch has its retention (§4.5), and claims grow with meaningful change, since a moved claim makes no new file. Repacking a knowledge base of millions of objects still has to be measured.
4. **An upstream that will not record.** If the upstream rarely calls `contribute`, E3 still keeps findings and demand, and E5's background reading of the files it read covers the rest. The evaluation set measures how often it records.
5. **One breaker for both classes** until home leaves background outcomes out of it (§8.2).
6. **Complexity inside askai's process.** Every engine step runs under the core's rules, and the stutter harness covers it.

## Appendix A. Words

| srtview | askai | Meaning |
|---|---|---|
| topic, `termN`, `focusN` | term | a name, and the matcher that finds it |
| semantic record | claim | an assertion with its evidence |
| — | assertion | a claim without its evidence: what verdicts judge |
| concept | merged claim | assertions that say the same thing |
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
| assertion | a claim's kind, relation, subject and object, without its evidence |
| bundle | what a query returns: terms, claims, findings, occurrences, gaps |
| claim | an observation of an assertion: its evidence and statement |
| contested | an object whose verdicts of the highest rank disagree |
| demand | decaying counters that order background work |
| entity | terms that verdicts join |
| evidence | lines of a file at a revision, with the quote that askai copied |
| finding | an answer kept with its validated citations |
| freshness | fresh, moved, changed or gone, at the current revisions |
| job | one request of the engine to the upstream |
| kb loop | the sidekick loop that owns the knowledge base |
| matcher | the pattern derived from a term's spellings and written pattern |
| occurrence | a line where a term's matcher matches |
| rank | a person's or a model's, which decides between verdicts and values |
| recipe | the prompt, schema, model and settings of a kind of job |
| source | a git repository that askai lists |
| term | a named thing: name, kind, spellings, pattern |
| testimony | what a person stated, stored as evidence |
| verdict | a judgment about one assertion, or about two assertions or terms |
| view | the immutable in-memory index of one commit of the knowledge base |
| window | lines of one file at one blob, which a job reads |
