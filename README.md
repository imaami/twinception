# askai

`askai` is an [MCP](https://modelcontextprotocol.io) server whose tool,
`ask_ai`, forwards a prompt to another AI model and returns its answer. A
small local model in `llama-server` can use it for a second opinion, or to
hand a hard sub-problem to a bigger model. The other model receives only the
prompt, so the tool description states that the prompt must be self-contained.
With `--man`, a second tool, `man`, reads the manual, and with `--git`, a tool
`git` runs read-only git commands in the repositories it lists (below). With
`--upstream-tools`, the other model may call those tools too, and keep notes
of its work, in a loop of turns that ends with its answer. With `--notes`, a
tool `notes` provides notebooks to clients too. With `--client-jobs`, clients
get the tools `background`, `jobs`, `wait` and `timer`, with which a client
runs its calls as jobs while it does other work (below). A plugin that the
configuration file lists adds tools of its own. The member `tools` of the file
can contain the objects of the built-in tools with their options, and can deny
tools to clients or to the other model (below).

[`askai-cli`](doc/cli.md) is a standalone terminal chat client for that
other model, `askai`'s upstream, connected to `askai` through a Unix socket.

The [session architecture memo](doc/session-architecture.md) records the
direction for shared notes, stable members, and multiple execution routes.

## Dependencies

- C23 compiler
- libcurl
- json-c
- GNU Readline (for `askai-cli`)
- utf8proc (for `askai-cli`)
- OpenSSL 3.5 or later (libssl, libcrypto), and its `openssl` command for the
  tests' certificates
- Linux epoll + timerfd
- libclang 19 or later, with its headers and `llvm-config`, for `askai-code`
  and the tool `code` (optional)

## Build

```sh
make            # builds askai, askai-cli, askai's plugins and askai-code
make askai-cli  # only the client
make askai-code # only the helper of the tool code
make check      # end-to-end tests, the dstr trials and check-load
make check-cli # only the terminal client's checks
make check-dstr # only the dstr trials
make check-load # only askai's load verdict on a light mix (10-20 s)
make load       # askai's load verdict on the storm (doc/core-measurements.md)
```

`make` builds `askai-code` only where an `llvm-config` of libclang 19 or later
is found; `make LLVM_CONFIG=llvm-config-19` picks one.

To chat directly with the model configured in an existing AskAI configuration:

```sh
build/askai --config /path/to/askai.json --listen unix:/path/to/askai.sock
build/askai-cli --socket /path/to/askai.sock
```

The client uses GNU Readline for terminal input, with searchable history,
PgUp/PgDn prefix matching, and completion of commands, file paths, exposed
tools, tool parameters and the members of the session. `/tools` lists the
client's permitted tools; `/tool NAME key=VALUE ...` calls one directly.
Tool activity appears as IRC-style action lines. It sends each chat turn
to AskAI, where tools and notes run, and contains no local inference engine.
See [terminal chat](doc/cli.md) for keys, discovery, commands and scripts.

## MCP transport

`askai` implements MCP's Streamable HTTP transport, which is what the
`llama-server` web UI's MCP client uses, and serves both protocol eras on one
endpoint:

- revisions 2024-11-05 through 2025-11-25, which open with an `initialize`
  handshake (current llama.cpp and the official SDKs);
- revision 2026-07-28, which is stateless and checks the mirrored
  `MCP-Protocol-Version`, `Mcp-Method` and `Mcp-Name` headers against the body.

A tool call answers on an event stream (SSE) when the client accepts
`text/event-stream` and its revision has them (all but 2024-11-05): the stream
opens as the call starts, carries a `:` comment every 15 s while the other
model works (`--sse-keepalive`), and ends with the result as one `data:`
event. A call whose `params._meta` has a `progressToken` also gets
`notifications/progress` on its stream, at most one every 100 ms: its place in
its session's queue while it waits for the upstream, then the tokens of the
answer as they come, "thinking" while the latest is the model's thinking and
"generating" once it is the answer's text. Other requests, and clients that accept only JSON, get
JSON. Request bodies may come with a `Content-Length` or in chunks.

A 2026-07-28 client that takes tasks, as its request's client capabilities
declare the extension `io.modelcontextprotocol/tasks`, gets a task of a tool
call that has not answered within a second: the call answers with the task's
id and goes on without the client, which polls `tasks/get` for where it
stands and, once it has ended, its result or error, or ends it with
`tasks/cancel`. A task's id is 128 random bits. Each task has an owner:
the identity of the client that started it, or the Unix user of a client of
a Unix socket. The owner of an anonymous client's task
over TCP is the client's address (IPv4, or the /64 of IPv6). Only the owner
of a task can read it with `tasks/get` or cancel it with `tasks/cancel`.
askai removes a task an hour after its call ends. It removes ended tasks
sooner in three cases. With 64 MiB of results, it removes the oldest ended
tasks first. With 1024 tasks, a new task makes it remove the oldest ended
task of the owner with the most ended tasks. When the owner of 64 tasks
starts another one, askai removes the owner's oldest ended task. A job that
a client starts with `background` (below) is a task too, and its owner can
read it with `tasks/get` and cancel it with `tasks/cancel`.
`server/discover` lists the extension; askai asks for no input, so
`tasks/update` has none to take.

## Upstream providers and APIs

`--api` says how the requests and answers are written:

| `--api`        | API                                                       |
| -------------- | --------------------------------------------------------- |
| `chat`         | Chat Completions: OpenAI's, and the many servers that copy it |
| `responses`    | OpenAI Responses                                          |
| `messages`     | Anthropic Messages                                        |
| `generate`     | Gemini `streamGenerateContent`                            |
| `interactions` | Gemini Interactions                                       |

`-p`, `--provider`, says who serves it, whose default API, URL for the API and
key variables are then the defaults:

| `-p`         | Default API    | URLs of its APIs (without `-u`)                                                   | Key variables                            |
| ------------ | -------------- | --------------------------------------------------------------------------------- | ---------------------------------------- |
| `llama`      | `chat`         | `http://127.0.0.1:9931/v1/` + `chat/completions`, `responses`, `messages`         | `LLAMA_API_KEY`, if set                  |
| `openai`     | `responses`    | `https://api.openai.com/v1/` + `responses`, `chat/completions`                    | `OPENAI_API_KEY`                         |
| `anthropic`  | `messages`     | `https://api.anthropic.com/v1/` + `messages`, `chat/completions`                  | `ANTHROPIC_API_KEY`                      |
| `google`     | `interactions` | `https://generativelanguage.googleapis.com/v1beta/` + `interactions`, `models/MODEL:streamGenerateContent?alt=sse`, `openai/chat/completions` | `GEMINI_API_KEY`, `GOOGLE_API_KEY` |
| `deepinfra`  | `chat`         | `https://api.deepinfra.com/v1/openai/chat/completions`                            | `DEEPINFRA_API_KEY`, `DEEPINFRA_TOKEN`   |
| `openrouter` | `chat`         | `https://openrouter.ai/api/v1/chat/completions`                                   | `OPENROUTER_API_KEY`                     |

The choice goes one way: a provider brings its API, URL and key, while `--api`
alone brings none of them. Without a provider, `-u` must say where the server
is, its API is `chat` unless `--api` says otherwise, and its key is
`ASKAI_API_KEY`'s, if that is set; `chat` covers every OpenAI-compatible
server this way: vLLM, Ollama, LM Studio, Groq and so on. With a provider,
`-u` replaces the provider's URL, and a provider that does not serve the API
asked for is refused. A provider's own URL needs `-m`, but llama's.
Reasoning or thinking output is dropped; only the answer is returned. An answer cut off by
the token limit is marked as such, and upstream failures (HTTP errors, error
events, refusals, timeouts) come back as tool errors the model can read and
react to.

Every API asks for a streamed answer: `stream: true` in the body, which
`-x` cannot turn off, and for Gemini's generateContent
`:streamGenerateContent?alt=sse`, which a
`-u` URL ending in `:generateContent` is asked as instead, with `alt=sse`
whatever `alt` it had. The tool still
returns the whole answer at once; streaming lets `askai` report progress,
notice a stalled answer (`--chunk-timeout`, 60 s between chunks once it has
begun) and cancel a synchronous call when the client goes. With
`--background`, the call and its upstream stream instead belong to a job,
which continues after the client's request ends. A server
that answers with plain JSON all the same is understood too.

API keys are read from the environment, never from the command line. A
provider's usual variables are only read for its own URL: with `-u`, the key is
`ASKAI_API_KEY`'s, or that of the variable `-k` names, so a provider's key is
never sent somewhere you did not ask for.

```sh
# Ask an Anthropic model.
export ANTHROPIC_API_KEY=...
./askai -p anthropic -m MODEL

# Ask the model of the local llama-server, at 127.0.0.1:9931 (no key needed).
./askai -p llama

# Ask a model served by a second llama-server.
./askai -p llama -u http://127.0.0.1:8081/v1/chat/completions

# Ask a model on OpenRouter.
export OPENROUTER_API_KEY=...
./askai -p openrouter -m MODEL

# Ask a Gemini model, in Gemini's Interactions or generateContent API, or in
# Google's chat completions.
export GEMINI_API_KEY=...
./askai -p google -m MODEL
./askai -p google --api generate -m MODEL
./askai -p google --api chat -m MODEL
```

`-x` deep-merges a JSON object into every request body, which reaches any
provider-specific parameter; `null` members delete keys. For example, OpenAI
reasoning models want `max_completion_tokens` instead of `max_tokens` in chat
completions, which `-p openai` uses only with `--api chat`:

```sh
./askai -p openai --api chat -m MODEL -n 4096 -x '{"max_tokens": null, "max_completion_tokens": 4096}'
```

## Asking without keeping the client connected

`--background`, or `"background": true` in the configuration file's
`upstream` object, makes an ordinary ask call return a job ID and
instructions to check back without waiting for generation. Its admission
record reaches a synced private recovery checkpoint before that reply. It
is off by default. For example:

```sh
./askai -p openai -m MODEL --background --tool-name ask_gpt
./askai -p google -m MODEL --background --tool-name ask_gemini
```

The client starts work with the usual arguments:

```json
{"prompt": "Investigate this problem: ..."}
```

It checks the job by calling the **same tool on the same AskAI instance**,
without repeating the prompt:

```json
{"job": "ID_FROM_THE_FIRST_REPLY"}
```

While work continues, this returns a successful pending message and asks
the client to check again later. Once the job ends, it returns the original
answer or error. `{"job": "ID_FROM_THE_FIRST_REPLY", "action": "cancel"}`
cancels the job. `action` defaults to `result`. Do not combine `job` with
`prompt`, `system` or `conversation`. The tool's description explains this protocol,
including when `--tool-description` supplies its other instructions.

Neither `--client-jobs` nor a grant of the generic `jobs` tool is needed.
Separate instances can use distinct ask tool names, so a follow-up reaches
the instance that owns the job. The ID is AskAI's own 128-bit random ID;
only the same owner can retrieve or cancel it. Client disconnects,
including a disconnected result check, do not cancel an accepted job.
AskAI keeps consuming the upstream deltas and stores the completed result
in its job ledger. The same live limits apply as to
[client jobs](#background-jobs-and-timers-for-clients): eight running jobs
per owner and results retained for up to an hour, subject to the memory
and record limits. Background mode also enables automatic transcript
storage, described below. After a restart, poll the same job ID using the
same service name (`--service-name`), transcript repository and caller
identity. AskAI loads
the saved state on that check; retained completed checkpoints remain
recoverable after the live ledger has forgotten them.

For OpenAI Responses and Gemini Interactions, AskAI sends both
`"background": true` and `"stream": true`. Native Gemini background
requests default to `"store": true`, which the provider requires for
background interactions. OpenAI Responses and synchronous Gemini
requests keep the `"store": false` default. `--extra` can change `store`,
but cannot disable `background` or `stream` in this mode; explicitly
forcing `"store": false` can cause Gemini to reject the request.
An explicitly selected `responses` or `interactions` API with a URL and no
provider uses the same native behavior.
Other provider/API combinations keep the stream locally without adding an
unsupported `background` field. With `-p llama`, AskAI assigns a unique
`X-Conversation-Id` to each provider turn. A llama-server that implements
the resumable stream extension can continue that generation after the
connection closes, and AskAI can replay it from the beginning.

Detaching a normal ask call does not lower its scheduling priority. The
existing `background` wrapper still requests background priority, but
native provider background calls are never preempted and restarted: that
would create a second provider job while the first still ran.

The configured total and chunk timeouts still apply. An explicit
cancellation also attempts the provider's cancel endpoint when its ID is
known. That attempt is bounded and may fail; closing a native background
stream alone does not cancel the provider's work. Shutdown preserves
durable background work for recovery instead of cancelling it upstream.
An explicit `action: "cancel"` still cancels it.

On recovery, AskAI requests the original provider job's stream from its
beginning and reconstructs the current turn in memory. OpenAI Responses
and Gemini Interactions are retrieved without a continuation cursor;
llama-server's `/v1/stream` uses the saved `conv_id` and `from=0`. It does
not repeat the original generation POST. A provider must still retain the
entire current turn's replay data. Missing or expired provider
state is reported as an error. Other Chat Completions servers remain
asynchronous while AskAI runs, but cannot resume after a restart without a
server replay API. An interrupted resumable connection suspends the job.
The next check of the same job ID retrieves and replays that provider job,
with the original deadline. There is no autonomous network retry loop.

llama-server's replay buffer is in memory and bounded. Restarting
llama-server, waiting beyond its retention period, or falling behind its
buffer can make reconnection impossible. Its current implementation can
also cancel a request disconnected before its first result. Durable
reasoning files preserve received thought text, but cannot reconstruct
missing answer or tool-call events that the provider no longer retains.

Recovered tool turns reuse saved tool results. AskAI records tool intent
before executing it and records its result afterwards. If a crash leaves
an intent without a result, recovery reports an unknown tool outcome and
stops; it does not execute a possibly completed side effect again.
A `findings` call commits its change to the session at once. AskAI
records neither its intent nor its result, and a recovered turn makes the
call again.
Interrupted tool jobs and timers are not restored, so recovery stops
explicitly when it would depend on them.

## Automatic transcripts

The transcript repository is separate from the shared notes repository;
neither repository may be inside the other's Git directory.
With `--notes PATH`, it defaults to `PATH.transcripts.git`. With
`--background` alone, it defaults to
`$XDG_STATE_HOME/askai/SERVICE/transcripts.git`, using
`$HOME/.local/state` when `XDG_STATE_HOME` is not an absolute path.
SERVICE is the name of the service, `--service-name`, which defaults to
`--agent`, or else to `--tool-name`. `--transcripts PATH` selects another
location and also enables recording without notes or background mode.
Startup logs show the path and the service. Use the same service name
after restarting; services of distinct names may share a transcript
repository, but two live instances cannot both write with the same
service name there. The second one stops with the message `the service
name NAME is in use in this repository; give this askai another
--service-name`.

Only decoded, concatenated reasoning text enters the branch
`reasoning/SERVICE`. No raw provider JSON, SSE envelopes, client
notifications, prompts, answers or tool results are added to Git.
Reasoning capture is independent of whether the client requests or displays
thought deltas, and also handles complete, non-streaming provider responses.

The writer appends each thought piece to a private plaintext file under
`askai-journal/SERVICE/reasoning/JOB_ID/TURN.txt`. It preserves the exact
decoded bytes, without adding separators between chunks. When the provider
closes a reasoning block, reasoning gives way to answer text or a tool call,
or the provider turn ends, the writer synchronizes the file and commits it.
Another reasoning block in the same turn appends to that file; an unchanged
file needs no new commit.
All filesystem and Git operations run on the writer thread, off event loops.

Unfinished files remain on disk through cancellation, failure and process
restart. Their writes have reached the operating system before the append
callback, but are not synced or committed per token; a power failure can
lose an unsynced tail. During provider replay, the writer compares bytes
against the existing prefix and appends only new bytes. A mismatch fails
that recovery and preserves the original file. A full writer queue applies
backpressure; storage errors are reported instead of silently losing text.

The file `askai-journal/principal.key`, outside Git and with mode 0600,
contains the 32-byte key of the principals. Every service of the
repository reads the same key (section Configuration file, identities and
roles, below).

Job recovery uses separate, private checkpoints under
`askai-journal/SERVICE/jobs/`, outside Git. They retain current request
state, provider identifiers, tool intents/results and terminal status;
superseded turn state and metadata are replaced. Checkpoints are bounded
to 64 MiB per job, 256 MiB per service and 1024 jobs, with space reserved
for one additional terminal record of up to 16 MiB plus its header. Atomic
replacement can temporarily use another 64 MiB. When space is needed, the oldest
completed checkpoints may be removed; unfinished jobs and checkpoints
migrated from the legacy journal are not evicted. These bounds do not prune
saved reasoning text or Git history. The notes tool exposes neither archive
nor checkpoints. The checkpoint of a background job contains the number of
the calls that its turns made, the evidence items that results showed, the
conversation, the ID of the ask call and the asker. A resumed job then
cites the items that it saw before the restart, and its commits name the
same ask call, conversation and asker. A checkpoint of an earlier version
resumes without the items, the ask call and the asker, and the log says
that its review record was not restored. A checkpoint whose number of
calls is not from 0 to 4294967295 is invalid, and its job does not resume.

Preserve the whole repository directory, including the private files and
owner seed, when moving or backing up a live session. A Git-only clone
preserves committed reasoning but cannot recover jobs or unfinished files.

Existing `transcripts/PARTICIPANT` branches and legacy journals are left
untouched. They are no longer extended or imported into Git at startup;
legacy job recovery can still read the old journal. The new branch starts
with a clean tree so it does not copy the old event directory into every
commit. This change stops new event-history growth but does not reclaim
existing disk usage. Ordinary `git gc` cannot remove history still reachable
from the old branches. Archive or remove that history separately after
preserving whatever it contains that you need.

## Connecting llama-server

Start `askai`, then add `http://127.0.0.1:8090/mcp` as an MCP server in the
`llama-server` web UI. The UI calls it straight from the browser, which `askai`
permits for pages served from `localhost`, `127.0.0.1` or `[::1]`. If the UI is
served from another origin, either allow that origin with `-o`, or start
`llama-server` with `--ui-mcp-proxy` and enable the proxy for this server.

Each tool call is one upstream request; if the user stops generation, the
web UI sends `notifications/cancelled`, or closes the call's connection, and
`askai` aborts the upstream request. `askai` asks the other `llama-server`'s
`/props` how many slots it has (`-np`), and makes as many upstream requests at
once; the rest wait their turn (`--slots` says how many instead).
The web UI waits five minutes for a tool result by default; `askai` lets a
call wait ten minutes for a slot and ten more for the upstream answer (`-T`).
Raise both for slow reasoning models.

Running several instances with different `--tool-name` values (and ports) gives
the model a choice of advisers, e.g. `ask_reasoner` and `ask_coder`.

## Reading the manual

With `--man`, `askai` also offers the tool `man`: a page of the manual of the
machine `askai` runs on, as `man [section] name` shows it, in UTF-8 text, so
that a model can check the interface of a system call, library function or
command rather than recall it. Its arguments are the page's `name` and an
optional `section`, such as `2`, `3`, `3p` or `7`. A name is 1 to 128
letters, digits and `_.:+@-`, and a section 1 to 16 letters and digits, neither
beginning with `-`, so neither can become an option or a path.

`askai` runs `man` itself, with no shell: `man -w -s SECTION -- NAME`, or
`man -w -a -- NAME` without a section, which
finds the page's file, then `man -l -- FILE`, which renders it. Each runs in a
session of its own, its standard error discarded, with an environment of its
own: `PATH` and `MANPATH` as `askai` has them, `LC_ALL=C.UTF-8`, `MANWIDTH=80`,
and `MANROFFOPT='-rHY=0 -dAD=l'`, which turns hyphenation and justification
off, as a model reads words split across lines, or padded with spaces, less
well. A page longer than 4 MiB is cut there with a note saying so; `gcc(1)`,
the longest page of many a system, renders to 1.4 MiB, which is some 370,000
tokens, more than many a model's context holds. An optional `offset`, in
bytes, starts the reply further on in the page.

With an optional `heading`, such as `SYNOPSIS`, `EINVAL` or `-Wall`, the
output is the part of the page under the least indented line that begins
with the heading followed by a space, a comma, `=`, `[` or the end of the
line, and `offset` counts within the part; a page without such a line is a
tool error that lists the page's headings, up to some 2 KiB. A longer list
ends with "and more headings." after the last heading that fits.

Without a section, a name with pages in two or more sections that are a
single digit, such as `open` in `1` and `2`, is a tool error that lists the
pages.

Each page is rendered once and kept, by its file: `printf` in section `3p` and
in `3posix` share one rendering, as do `S_ISBLK` and `inode(7)`, which the
former's file points to. Asked for again, a page comes from memory, in 0.3 ms
for `pthread_create(3)` and 5.5 ms for `gcc(1)`, as long as its file is as it
was; a file that has changed is rendered anew. A name `man` has found a file
for is found anew once that finding is ten minutes old, in the background,
the kept page given meanwhile, so that a page installed since, which `man`
would find first, takes over. Calls for a page under way wait for its one
rendering. Pages are kept for good: all of a system's pages take some 100 to
200 MiB. `GET /metrics` shows the pages kept and their bytes, the calls a
kept page answered and those that waited, and the runs of `man`.

A missing page is a tool error, "No manual entry for NAME in section S.", and
so are `man`'s other failures and a page that takes more than 10 s to find or
render; none of those is kept. For a missing page, `askai` also runs
`man -k -- '^NAME'` and names the pages whose names begin with the name, up
to some 4 KiB. A longer list ends with "and more pages." after the last page
that fits. At most 16 `man` and `git` processes run at
once; a call past those is answered as busy. A call that goes, as when its client closes
the connection, leaves the page to be rendered and kept; stopping `askai`
kills `man` and the processes it started.
If `man` exits while a process that it started still has the output open,
that process is still killed at the deadline or when `askai` stops. Until
the output ends, that `man` still counts toward the limit of 16 processes.

## Reading git repositories

With `--git PATH`, which may repeat, or the list `tools.git.repositories` of a
configuration file (the member `repositories` of the object `git` in the
member `tools`), `askai` also offers the tool `git`: read-only git commands in
the repositories listed, each a work tree's top or a git directory, and named
by the last component of its path as given, less a `.git` suffix:
`/srv/git/proj.git` is `proj`. A name is 1 to 64 letters, digits and `_.-`,
not beginning with a dot; a path without one, or two of one name, are refused
as `askai` starts. A call names its
`repo` and its `command`, one of `read`, `log`, `show`, `diff`, `grep`,
`blame`, `ls-tree`, `cat-file` and `rev-parse`, and gives its `options`, its
`revisions` and its `paths` apart, and for `grep` its `pattern`:

```json
{ "repo": "proj", "command": "log", "options": ["--oneline", "--max-count=20"],
  "revisions": ["main..topic"], "paths": ["src"] }
```

`blame` takes its file as the one path, and `cat-file` exactly one of `-p`,
`-t`, `-s` and `-e`.

Only committed content is visible. The default revision is `HEAD`, including
for `grep`, `blame` and `ls-tree`; a staged deletion does not hide its committed
file, and staged additions or edits, working-tree edits, untracked and ignored
files are absent. Use `ls-tree -r --name-only` to list committed paths. A `grep`
pattern is an extended regular expression (`-E`) unless the options give `-G`,
`-F` or `-P`. With one revision, the first line of the output of `grep` names
the repository and the commit: `[Repository proj, commit ID]`. Each file path
is on a line of its own, the matches in the file follow as `line:text`, and an
empty line separates the files. With several revisions, the commit ID and a
colon come before each path. `--no-heading` gives `path:line:text` for each
match instead. `-l` and `-L` list paths, and `-c` gives `path:count`. A commit
ID can replace `HEAD` for repeatable reads. `diff` requires two commit
revisions or one explicit range such as `main..topic` or `main...topic`.

`read` gives numbered lines of a committed file, which the one revision names
as `commit:path`, from `line` (default 1), `count` of them at most (default as
many as fit in one result):

```json
{ "repo": "proj", "command": "read", "revisions": ["HEAD:src/journal.c"],
  "line": 120, "count": 40 }
```

The first line of the output names the repository, the file, the full commit
ID and the lines given:
`[Repository proj, file src/journal.c at commit ID. Lines 120 to 159 of 900.]`.
Each line follows as its number, a tab and its text. The last line gives the
revision, with the commit ID in place of a name such as `HEAD`, and the `line`
to read on from, or says that the file ends there. The next part then comes
from the same commit, even if `HEAD` has moved. The commit that a `grep` names
and the path of a match make the `commit:path` revision of a `read`, and the
number of the match is the `line`. `show` with the same revision gives the file
as it is.

Use `"help": true` with a command to list its options, source selection and
argument limits, and the bytes of output that one result of the caller takes
(`window_bytes`: a turn's window, or 16 MiB for a client). The other arguments
of such a call, except `offset`, are ignored. Each option must be on that
command's allowlist, with its value attached (`--max-count=20`, not `-n 20`).
Revisions name commits, optionally with `^` or `~` ancestry; `log` and `diff`
also accept explicit ranges. Only `read`, `show` and `cat-file` take a path in a
revision, as `commit:path`; `ls-tree` lists the entries in a directory given in
`paths` as `src/`, with the slash. Index selectors such as `:path`, raw blob or
tree IDs, stash and reflog selectors, and revision-list expansion syntax are
rejected. `status`, `ls-files`, and index and worktree options are unavailable.
`log` also takes `--all`, `--branches`, `--tags` and `--remotes`. Every resolved
commit, including `blame --ignore-rev` values, must be reachable from `HEAD`, a
branch, a tag or a remote-tracking ref.
Knowing a stash or dangling commit's hash does not grant access. A commit
deliberately added to ordinary history becomes available through that history.
Paths are relative to the repository's top, with no `..` component
or `:(...)` pathspec magic; a pattern follows `-e`.

Once the options, revisions and paths are arrays of strings within their
limits, a refusal names a problem with each argument and entry that has one,
not only the first.

Option errors distinguish missing or malformed attached values, options outside
the selected command's allowlist, and unavailable index or working-tree sources.
Only value errors suggest changing how a value is attached; source errors explain
how to select committed revisions instead.

No option that writes a file (`--output`), reads a separate file (`grep -f`,
`blame --contents`, `--no-index`) or runs a program (`grep -O`, `--ext-diff`,
`--textconv`) can come from a client. `askai` runs `git` itself, with no shell,
in a session of its own, with an environment of its own (`PATH` as `askai` has
it, `LC_ALL=C.UTF-8`, a `HOME` that does not exist, no system or global
configuration, no terminal prompts, no optional locks, no repository looked
for above the listed ones, and no remote protocol or fetch of a partial clone's
missing objects). Each call uses a private bare view of the approved refs and
shared object storage, with no source worktree, index or local configuration.
Validated shallow-history boundaries are preserved.
Revisions are pinned to object IDs before execution; attributes and mailmaps
come from the view's fixed `HEAD`. Replacement objects, pagers, fsmonitor, gpg,
hooks and submodule diffs are disabled. Path names are printed as they are
(`core.quotePath=false`); only a name with `"`, `\` or a control character is
quoted. Repository metadata and object storage remain trusted: this view is
not an operating-system sandbox.

The reply is `git`'s output, made UTF-8 text and cut at 4 MiB with a note saying
so, from the byte an optional `offset` names. A revision that names no readable
commit gets one text, which lists the possible causes, so that a caller cannot
tell a missing hash from an unreachable one. A failed `read` or `cat-file` of a
`commit:path` says only that the commit has no file at that path, and other
failed `show` and `cat-file` calls return a generic committed-object error,
hiding Git's hints about local files; other Git errors are preserved. `read`
runs `git cat-file blob` with its errors discarded, numbers the lines within
the 4 MiB that `git` reads, and refuses a file with a NUL byte in its first
8000 bytes as binary. Nonzero exits are tool errors, except that a `grep` that
writes no line succeeds with a note: `[No matches.]`, with `-L`
`[Every file contains a match.]`, with `-v` `[Every line matches.]`, and with
both `[Every file contains a line that does not match.]`. At the deadline,
`--git-timeout` (30 s for validation and execution together), `git` gets
SIGTERM, and SIGKILL a second later, and the call is a tool error; a call that
goes, or `askai` stopping, kills `git` and the processes it started.
If `git` exits while a process that it started still has the output open,
that process is still killed at the deadline, when the client of the call
closes the connection, or when `askai` stops. Until the output ends, that
`git` still counts toward the limit of 16 processes.

## Asking clang about C code

With `--code REPO=FILE`, which can be repeated, or the list
`tools.code.compile-commands` of a configuration file, `askai` also offers
the tool `code`, which runs clang on the committed C code of the
repository `REPO` of `--git`, given by its name or by its absolute path
exactly as `--git` gives it. `FILE` is the `compile_commands.json` of the
repository, such as `build/compile_commands.json`, which `make
compile_commands.json` writes. A relative `FILE` is relative to the
directory of the repository. The tool requires `askai-code`, the helper
program that `make` builds next to `askai` if an `llvm-config` of libclang
19 or later is installed. A role whose tools a configuration file lists
gets the tool only if the list names `code`.
[`doc/configuration.md`](doc/configuration.md#code) describes the options,
the cache, the startup check and `--code-unconfined`.

The arguments of a call are `repo`, `action` and `path`, a `.c` or `.h`
file of the commit relative to the top of the repository, and optionally a
`revision` (in the form that the git tool accepts, `HEAD` by default) and
an `offset`.

`outline` lists the names that the file declares, one per line: the line
of the name, its kind, the type of a function, variable or typedef, and the
first and last line of the declaration if they differ:

```text
[Repository proj, file src/git_view.c at commit 1095916a86bd9dc0585d261ec87762fe6134de44. 13 names.]
5 macro _GNU_SOURCE
19 static variable git_view_template: const char[22]
24 struct git_view (24-28)
34 static function git_view_path: int (struct cstr_view *, struct cstr_view *) (33-39)
```

`describe` prints what `name` is in the translation unit of `path`. `name`
is a function, variable, type, struct, union, enum, enumerator, typedef or
macro, with `struct`, `union` or `enum` before it to select the type, or a
member as `struct.member`. The output contains the kind and the type, the
places of the declaration and the definition, the calls of a function
(below), the members of a struct or union with their offsets, sizes
and padding, including the members of anonymous members, and at most 40
numbered lines of the declaration with its comment, a function's without
its body. A finding can cite these lines (below). With `line`, `describe`
uses the name on that line of `path`, such as a local variable or a macro
use. For a macro use, it also prints the type and the value of the
expression. This is `describe` of `struct finding` in `src/finding.c`:

```text
[Repository proj, file src/finding.c at commit 1095916a86bd9dc0585d261ec87762fe6134de44. struct finding in the translation unit.]
struct finding: 232 bytes, alignment 8, 0 bytes of padding, x86_64-pc-linux-gnu
defined src/finding.h:111
 offset   size  align   line  member: type
      0     24      8    112  claim: struct dstr
     24     24      8    113  changes: struct (unnamed)
     24      8      8    113    items: struct finding_change *
     32      8      8    113    n: size_t
     40      8      8    113    cap: size_t
     48      4      4    114  number: uint32_t
     52     82      1    115  about: char[82]
    134     33      1    117  conversation: struct hexid
    167     65      1    120  author: char[65]
src/finding.h lines 110 to 121 of 303:
110	/** A finding. Each member has a defined value from finding() on. */
111	struct finding {
...
```

If a name matches several symbols, the call fails with a list of them. If
it matches none, the call fails with the names in the repository that
contain it. The layout of a record is cut at 2,000 rows, and the padding of
a cut layout counts only those rows. `describe` of `struct.member` prints
any one member.

`describe` of a function lists the calls of the function, one row per
callee: the place of the first call, the lines of the later calls, the
called function and the place of the first declaration of the called
function. This is the list of `mcp_req_stream` in `src/mcp.c`:

```text
calls:
  src/mcp.c:3033 indirect, through the member head of struct stream_ops (src/stream.h:52)
  src/mcp.c:3033 span (src/buf.h:58), through the macro SPAN (src/mcp.c:864)
  src/mcp.c:3036 mcp_req_flag (src/mcp.c:1021)
  src/mcp.c:3040 mcp_req_arm (src/mcp.c:2327)
```

A row is one of these:

| Row | The call |
| --- | -------- |
| `child_cmd (src/child.h:185)` | a call of the function `child_cmd`, also in parentheses or behind `*` or `&` |
| `__builtin_expect (builtin)` | a call of a builtin function that no file declares |
| `indirect, through the member head of struct stream_ops (src/stream.h:52)` | a call through a function pointer: a member, a variable or a parameter, or `an element of` one that is an array |
| `indirect, through an expression` | a call through any other expression, such as the result of a call |
| `hist_init (src/hist.h:64) or hist_clear (src/hist.h:130). The tool cannot tell which function is called.` | a call of a function that `?:` selects when the program runs, or that a `_Generic` selection selects |
| `dstr_cleanup (src/dstr.h:384), at the end of the scope of a variable (attribute cleanup)` | the call that the program makes for a variable with the attribute `cleanup`; the place of the row is the line of the variable |
| `a function, at the end of the scope of a variable (attribute cleanup). The tool cannot tell which function is called.` | the same call, if no declaration at file scope declares the function of the attribute |

The row of `?:` contains the functions of the second and the third operand,
also those of a `?:` in an operand, and not the function of the condition.
If an operand or a selected association is not a function, the row is
`indirect, through an expression`.

`, through the macro child_cmd (src/child.h:190)` after the callee is the
macro that is used in the file at the place of the call, if the first
token of the call is part of the body of a macro. For a macro in the
arguments of another macro, as in `LOG(WRAPPER(x))`, the macro is
`WRAPPER`. For a macro that is passed by name, as in `LIST(X)`, the macro
is `LIST`. A call that is written as a macro argument is in the row of the
callee without a macro.

libclang does not report which association of a `_Generic` selection is
selected. The selection is of the type of the selected association, so the
selected association is known if no other association is of that type. If
the callee is a selection of several functions of one type, the row
contains the functions. The row of a call inside a selection whose
selected association is not known ends with `The tool cannot tell if the
program evaluates the call.` So does the row of a call in an operand of
`sizeof` whose value is not a constant, of a call in an operand of
`typeof` whose type can be variably modified, such as a pointer or an
array, and of a call in an argument of `__builtin_constant_p`,
`__builtin_object_size`, `__builtin_dynamic_object_size`,
`__builtin_classify_type`, `__builtin_assume` or `__assume`: clang does not
evaluate the arguments of these builtin functions for side effects.

A call in code that the program does not evaluate is not listed. Such code
is an operand of `sizeof`, `alignof` or `typeof`, the controlling
expression of `_Generic` and an association that is not selected, and the
condition of `__builtin_choose_expr` and the operand that is not chosen.
`describe` prints the number and the places of these call expressions in a
line after the rows:

```text
[The program does not evaluate 1 call expression at src/store.c:409. The rows do not contain this call expression.]
```

A variable in such code is not listed for the attribute `cleanup`.

The list is made from one translation unit with the flags of the build:
code that `#if` removes contains no call. A call in code that the program
cannot reach is listed. libclang does not report an expression inside the
type of an association of `_Generic`, such as `typeof(f())`, or inside an
attribute, such as `__attribute__((assume(f())))`, so a call there is
neither listed nor counted.

An operand of `typeof` is recognized by the keyword that is written before
the parenthesis, with only white space between them. A call in an operand
is listed as a call that the program evaluates if a comment or a line
continuation is between the keyword and the parenthesis, if the keyword is
the body of another macro, or if the keyword is on a line before the
parenthesis and that line contains `#` or `//`. The controlling expression
of `_Generic` is recognized by the first token that is written right after
`_Generic (`. If that token is a macro argument, the expression is handled
as an association: the row of a selected callee can contain a function of
the controlling expression.

`diagnostics` lists clang's errors and warnings for `path` in clang's order,
one per line as `path:line: warning: text [-Wflag]`, including those in the
headers that it includes, without notes or columns. `warnings` adds up to
16 warning options to those of the build, each `-W` or `-Wno-` and the
name of a warning, optionally with `=DIGITS`. A file outside the tree that
cannot be opened is reported as not found.

Every result starts with the repository and the full ID of the commit.
Places are `path:line`, relative to the top of the repository, or absolute
for a system header. The `read` command of the git tool accepts the commit
and the path.

Only committed files are visible. A call resolves its revision as the git
tool does, and `askai-code` exports the commit into a tree in its cache.
The files of the tree contain the bytes of the blobs: no attribute, filter
or hook applies, and a symbolic link is a file that contains its target.
An export of another commit rewrites only the files that differ, and the
cache can be removed while `askai` is not running. A commit of more than
100,000 files and directories, or 1 GiB, is refused.

`askai-code` reads `FILE` at each call, so the results use the flags of the
last build. Only entries with `arguments` are used, as written by the build
of `askai`. A path at or under the top of the repository, including the
value of `-I` or `-include`, is mapped to the same path in the tree, and an
option with a path above the top is removed. The options that write files
(`-o`, the `-M` options, and `-Wp,` with anything but one `-D` or `-U`),
`-Xclang`, `-Xpreprocessor` and `-mllvm` with their values, and response
files are removed. A `.c` file that is not in `FILE` is refused. A header
is parsed with the command of the listed file nearest to it and
`-x c-header`.

`askai-code` runs once per call with the environment of the git tool, and
clears the environment before the parse. Before the parse, it also
confines itself. It sets no_new_privs, disables core files and file
writes, and limits its memory to 1 GiB. With Landlock, it can read only
`/usr`, a few system files and the tree, and write only `/dev/null`. With
seccomp, it cannot open a socket, run a program, signal or change another
process, or change the attributes of a file. A committed `#include` of a
file outside the tree fails, but the commit can learn whether the file
exists. clang's debug pragmas are disabled. A parse that crashes, such as
one of deeply nested expressions, ends the call with the signal. At
`--git-timeout` (30 s for resolving the revision and the parse together),
`askai-code` is killed with SIGKILL, and its git processes shortly before.
The result is at most 4 MiB, and `offset` continues past the end of one
result.

## Lending tools to the upstream

With `--upstream-tools`, an `ask_ai` call offers the other model tools of its
own, in any of the APIs (`llama-server`'s chat template must take tools, as
with `--jinja`): those of `man`, `git` and `code` that the caller's role lets
the upstream call (`ask_ai` itself is never offered), `notes` with `--notes`,
and with any of those `compact`, `jobs`, `wait` and `timer`, `findings` too
with `--notes`, and `background` beside a tool that can run in the
background. A plugin's tool
is offered in the same way if its kind allows it (see Plugins below).
Offered none, the call asks once, as without the option. Else it runs a
loop of turns: each turn is an upstream call, whose answer may call tools;
`askai` runs them at once (a `notes`, `compact`, `findings`, `jobs` or `wait`
call keeps to itself, after the calls before it and before those after it),
writes the turn and their results, in the calls' order, into the
conversation, and asks again, until an answer calls none, which is the call's
answer. A turn goes back as it came where the API asks for that: a call's
`extra_content` in chat completions, in which Gemini's give their thought
signature; the output items of the Responses API, its reasoning among them;
the thinking blocks of Anthropic's Messages, with their signatures, in their
places among the text and the calls; Gemini's parts, with their thought
signatures; and the steps of Gemini's Interactions, thoughts and calls with
their signatures among them. Each turn waits for an upstream slot like any
call, so the scheduler's fairness holds
turn by turn. The caller gets the answer, and a second text that lists the calls the turns made, with their arguments (but
those of `notes`, `compact` and `findings`, which are the model's own) and the bytes of
each output. If the turns wrote a note or a finding, the second text ends
with a line such as "bro wrote in the session." With `notes`, the line
ends with the conversation: "bro wrote in the session. Give conversation
6817…6172 to continue the work." An `ask_ai` call with `conversation`
continues the conversation, and one without starts a new conversation. The argument `conversation` needs `notes`. The
argument `thread` of earlier versions is refused, and the refusal names
`conversation`. With a `progressToken`, its notifications say the turn and the
tool being called. `--max-turns` (32) bounds the turns: the turn before the
last is told so, and a last answer that still calls tools ends the call, as an
error if it has no text. A `notifications/cancelled` from the caller of the
call ends it too, at once, also while a tool runs or the turns doze. The
upstream must stream
its answers, as `llama-server` does; a strict server that refuses
`stream_options` takes `-x '{"stream_options": null}'`, which leaves the
context to be counted by the request's bytes.

A tool's output goes to a turn in a window of an eighth of the context, at
most 64 KiB, with a note that names the bytes shown and the `offset` that
reads on with the same arguments. The window starts at the start of the line
that contains the `offset`, and ends at a line's end, where these are within a
quarter of the window. The tools that take an `offset` state the window in the
descriptions that a turn gets. A `git` `read` gives the whole lines that fit
the window, and names the `line` to read on from instead; only a line longer
than the window is cut by bytes. The context is what `--context` says, or else
what `llama-server`'s `GET /props` says (`n_ctx`), which `askai` asks for even
with `--slots`, or, with Gemini's APIs and a model, what `GET
/v1beta/models/MODEL` at the root of Gemini's API in the URL says
(`inputTokenLimit`); each turn asks for its usage
(`stream_options.include_usage`), which counts the tokens. With `--notes`,
`askai` adds a reminder to write notes to the last result of a turn's tools,
once when the context is a quarter full, and once when it is half full.
Compaction removes the conversation but not the notes, so notes that the model
writes early are still there after it. From two thirds of the context on,
`askai` adds the fill level of the context to the last result of each turn's
tools, with a request to note what the model needs and to call `compact`.
After a compaction, `askai` adds the reminders again as the context fills up.

The note of each result of a turn's call contains the number of the call,
such as `[askai: call c7]`, after the notebook notice. The calls of an ask
call are numbered from 1 in the order of the calls, across turns and
compactions, whatever order the calls end in. A failed call and a refused
call get a number too. The calls after `compact` in the same turn do not
run, but their numbers are used. The notices about the context, the last
turn, jobs and timers follow the number. A background job resumed after a
restart numbers its next calls after the calls of its checkpoint.

If the turns get `findings`, `askai` stores the last 64 calls of each ask
call in memory, at most 16 MiB of them, and drops the oldest call first.
For each call, `askai` stores the arguments as the model sent them, the
result as the model got it, and for a `git` `read` or a `code` `describe`
the commit, the path and the lines that the result showed whole. A call
larger than 4 MiB is stored as its number only. A `findings` or `compact`
call is not stored, because it shows nothing that a finding can cite.
`askai` also stores the evidence items of findings that `findings` printed
to the turns, at most 1024, with the bytes of each item that results
showed. The stored calls and items are dropped when the ask call ends.
After a compaction, a notice names the stored calls that a finding can
cite, and another the items that the turns saw.

The model need not wait for a slow call: `background` runs a call of `man`,
`git` or a plugin tool that allows it, as a job, and answers at once with
its number, 8 jobs a call at most; `timer` sets a timer of up to an hour,
with a note. Jobs and timers have numbers of their own, each from 1. `jobs`
lists the jobs and the timers that are set, gives an ended job's output, a
window at a time from its `offset`, or cancels a job or a timer. A job that
ends and a timer that fires are told, in brackets, after the next result the
turns are given; `wait`, for up to 10 minutes, ends as soon as either
happens, a job of those it names or any, and says what did, which takes no
upstream slot while it waits, as polling `jobs` would. `background` comes
with a tool that can run so, `jobs`, `wait` and `timer` with any.

An answer while a job runs or a timer is set does not end the call: the
turns doze, holding no upstream slot, until a job ends or a timer fires,
or at once if one did as they answered; then a message in the user's place,
after the answer, says what happened, and they go on. The reply gives each
answer, and what woke the turns after it. An answer with nothing under way,
or in the last turn, ends the call, and its jobs and timers with it.

`compact` takes a summary: the conversation after the prompt, its turns and
the tools' results, goes, and `askai` adds to the prompt, in brackets, the
summary, the note history, the current text of the most recently written
note, and with `findings` the open findings of the agent in the
conversation and the calls that a finding can cite; calls after `compact` in the same turn
do not run.
Should the context fill up all the same, as the upstream says (llama-server's
"exceeds the available context size", or a "context length" or "context
window" error) or as the usage counts, `askai` does likewise without a
summary, once until a turn goes through again; the next overflow is the
call's error.

With `--notes`, `findings` keeps the findings of the agent on the branch
`refs/heads/agent/NAME/findings` of the session. A finding is a claim of
the agent about material that `askai` showed it, with an ID such as
`bro#5`: the name of the agent and a number that counts its findings from
1. The ID stays the same in every ask call. `add` records a finding with
a claim and a state, `candidate` unless the model gives another, and with
`about` the ID of the finding of another member that it checks. `update`
changes the state of a finding of the agent to `candidate`, `verified`,
`disproved` or `retracted`, and with `claim` corrects the claim; the
change stores the earlier claim. Only the author changes a finding. `list`
lists the findings of every member, open first, or with `author` those of
one member. `show` prints a finding with every change, and the findings
about it. `expand` prints the stored evidence of a finding (`id` such as
`cuh#2`), of one evidence item (`cuh#2/1`), or without `id` of all open
findings of the agent, newest first, a window at a time from `offset`.
The state of a finding is the state of its last change, and `candidate`
and `verified` findings are open.

A change cites evidence, at most 16 references: `c7` is the call c7 of
the ask call, its arguments and its result as the model got them, or the
lines of a `git` `read` or a `code` `describe` that its result showed
whole; `c7:120-140,150` is some of those lines; `cuh#2/1` is the
evidence item 1 of the finding `cuh#2`, and
`cuh#2/1:161-165` some lines of such an item. The ranges can come in any
order. `askai` stores them in ascending order, ranges that overlap or touch
as one range, such as `120-140,141-150` as `120-150`, and their lines in
that order, each line once. `askai` stores the
material in the same commit as the finding, each as `archive/XX/YYY` named
by the SHA-256 of its bytes, and the file of the finding names it with the
identifiers that reproduce it, such as the repository, the commit and the
path. An item cited again contains the identifiers of the item and names
it, and its archive files are the Git objects of the other finding, so
each branch contains all of its evidence. `external` names material outside
`askai` with an exact hash, at most 8 items, which `askai` does not
store. `verified` and `disproved` need a `reason` and at least one
reference that is not external. Each call that they cite must be of an
earlier turn, and each item must be seen: a result of an earlier turn of
the ask call showed the last byte of the item, after its other bytes in
order, as the pages of `expand` do. `show` does not count. The refusal of
an item that the turns had not seen prints the item if it fits in one
result, and the item then counts as shown in that turn. `retracted`
needs a `reason`. A call that left the memory of the ask call, or that
`askai` made before a restart, cannot be cited, and the refusal says why.
An item that a result showed before a restart stays seen. A background
job that `askai` resumes after a restart makes the `findings` calls of the
interrupted turn again. An `add` or `update` that made its commit before
the restart finds that commit by its lines `Ask:` and `Call:` among the
newest 64 commits of the branch, answers such as "bro#5 was already added
in commit 3e1f0a2b before askai restarted.", and makes no second commit.

`expand` reads the file of the finding and its archive files with `git
cat-file --batch`, so it works on every service of the session, and a
notice names stored material that is missing or that does not match its
SHA-256. One call prints at most 8 MiB of material and names the items
that did not fit. Clients can call `findings` too, with `--notes`, as
their role allows: `list`, `show` and `expand`, but no change.

Each `add` and `update` is one commit on the branch of the agent, by the
agent, with the trailer lines `Ask:`, `Call:`, `Conversation:` and
`Asked-by:`. The file of a finding is `findings/D/N.json`, with `D` the
number divided by 100, a JSON object of version 1. The number of a new
finding is one more than the highest number on the branch when the commit
is made. If another ask call of the agent changes the branch at the same
time, `askai` reads the branch again and makes the commit again, three
commits at most. Each ask call and each compaction starts with the open
findings of the agent in the conversation, newest first, within a tool's
window, and the number of its open findings in other conversations, also
for turns that get `findings` without `notes`. Such an ask call starts a
new conversation, because the argument `conversation` requires notes.
The notice after the summary of a compaction states that the system text,
the client's messages and the prompt stay word for word, so the model
need not copy the task into notes. Nothing prunes the branch or the archive
files of the evidence, just as nothing prunes notes. Run `git gc` as
needed. The review records of earlier versions under `refs/review/`
stay where they are, and `askai` no longer reads them.

With `--notes PATH`, the repository at `PATH` is the shared session.
`askai` creates a bare Git repository there if absent, and also accepts an
existing ordinary repository or worktree. All instances
using that repository collaborate on its notes. `--notes` needs `--agent
NAME`, the name of the agent of the instance in the session. A member of
the session is a person or an agent. Persons and agents share the names
of a session: 1 to 64 characters from `[A-Za-z0-9_.@-]`, not starting
with `.` or `-`, without `..`, not ending with `.` or `.lock`, not `@`,
and not a letter followed only by digits, such as `c7`.

As it starts, `askai` writes the session record `refs/askai/session`, with
the ID and the name of the session, and the member record
`refs/askai/members/NAME` of its agent, if they do not exist. A member
record is a create-only ref: the first writer of a name creates it, and a
later service of the same agent adopts it. A person becomes a member with
the first write of `notes`, or with the first ask call whose turns get
`notes`. The `_meta` of the tool `notes` has the ID of the session as
`session`, the name of the agent as `agent`, and `members`, which says that
the tool lists the members.

Each member has one notebook, the branch `refs/heads/person/NAME/notes` or
`refs/heads/agent/NAME/notes`, and writes only that notebook. Every member
can read every notebook. The member of a call depends on the caller:

- The turns of an ask call write as the agent of the instance. Every
  service of one agent writes the notebook of the agent.
- A client on a Unix socket writes as the person of its Unix user, named
  like the user, or `uid-N` if the user has no valid name. If the lookup
  of the name fails in another way, such as after 10 s, the write fails
  and asks to try again. The record of the person names the login, so a
  renamed user keeps the notebook.
- A client over TCP with the token of an identity writes as a person named
  like the identity, or as an agent if the identity has `"agent": true`.
  An identity and a Unix user of the same name are one person.
- A client over TCP with `ASKAI_TOKEN` under `askai`'s own policy writes as
  the user who runs `askai`.
- A client over TCP without a token has no member name. It reads, but it
  cannot write notes.

A login whose name is the name of a member of another kind is refused,
such as a Unix user named like an agent, and the refusal says so. The
repository, rather than an ask call or transport connection, determines
which session a note belongs to.

A note is a file. The model chooses its name, such as `plan.md` or
`design/locks.md`, and writes its text, at most 4 MiB. The extension of
the name shows the format of the text, which `askai` does not check.
The tool `notes` creates a file with `add` or updates an existing file with
`replace`, or with `append`, which adds text at its end, on a new line if
the file does not end with a line feed. `delete` removes a file; `restore`
with the commit before the deletion brings it back. Each write that changes
a note adds a commit to the notebook of the caller with `git fast-import`.
The author is the member, such as `bro <agent/bro>` or
`imaami <person/imaami>`, and the committer is the service, such as
`gpu1 <askai>`, so `git log --author='^bro <'` lists the changes of a
member. The message says what the write did, the note's name and its size
before and after, such as `Replace "plan.md" (1234 to 1302 bytes)`;
previous versions remain in history. The commit of a turn ends with the
trailer lines `Ask:`, the ID of the ask call, `Call:`, the number of the
turn's call that made the commit, `Conversation:`, its conversation, and
`Asked-by:`, the member whose client made the ask call, if it has a name.
`history` and the prompt show the asker as `, asked by NAME`. The
result names the new commit and repeats its message. If the write changed a
note, the result also names the commit of the previous version, after the
lines that a `replace` or `restore` removed and added, such as `1 line
removed, 2 added.`, unless Git sees the note as binary. A `replace` or
`restore` that would leave the note's bytes as they are makes no commit; its
result says `Unchanged` and names the notebook's newest commit. `add`
refuses an existing name, and `replace`, `append` and `delete` a missing
one. If another call changes the notebook during a write, askai reads the
notebook again. If the note and each directory in its name are as the write
found them, askai makes the same commit on top of the other change, with at
most 3 tries in all. Otherwise the write fails, and its result says to read
the current note and retry. No write overwrites another writer's changes.
When a turn writes text whose last lines are askai's own notices from tool
results, such as `[askai: notebook NAME (KIND)]`, `[askai: call c7]` or the
context notices, askai leaves those lines out and says so in the result. In
a notebook notice, NAME and KIND can each be any valid member name. Notices
inside the text, and text from clients, stay as they are. askai adds a line
feed to a turn's text that does not end with one. askai refuses a turn's
text that contains `</function>` and later `</tool_call>` or `<tool_call>`,
and writes nothing. The text argument of such a call ended early, and the
next tool call of the turn became part of the text. Text from clients is
not checked.
Names cannot contain control characters or empty parts. No part can be `.`,
`..`, `.git` or `git~1`, in any case and also with spaces or dots after it,
because git refuses such names. Slashes at the start are left out. A role
that names `notes` among its `tools` gives clients their notebooks with
`--notes` alone. With `--upstream-tools`, the model gets `notes` whatever
the role lists. `git` runs without hooks. Nothing prunes the repository; run
`git gc` as needed.

Branches can have separate Git worktrees. Writes made through the notes
tool update Git objects and the branch; they do not update files in an
already checked-out worktree. Refresh a clean worktree explicitly before
reading those files. A nonempty directory from the older per-thread
layout is not migrated automatically: keep it and choose a new session
repository path.

Files committed through a worktree appear in the notebook, including
several files added by one commit. `list` reflects the current tree;
`history` retains the record of changes.

- `members` lists the members of the session, persons first, each with its
  kind and the newest change of its notebook, or `no notes yet`. Its last
  line says who the caller is. For a client, the result also has
  `structuredContent`: `members`, each with `name`, `kind`, and for a
  notebook with notes `time` and `change`, and for a caller with a member
  name `you`, with the name and the kind of the caller. A notebook or a
  branch of findings without a member record, such as a notebook moved
  from the old layout, lists its member with the kind of its branch.
- `notebook` names the member whose notebook an action reads. The default
  is the notebook of the caller. A name that no member has is refused with
  the names and the kinds of the members, at most 32, and the number of
  the others. A notebook without notes answers that the notebook contains
  no notes yet; for `list`, `history` and `search` the answer is not a
  failure. Writes go only to the notebook of the caller.
- The prompt of an ask call begins its notes with who the agent is and who
  made the ask call, then the members of the session and their kinds, at
  most 32, and the number of the others, then the note history of the
  agent and its newest note, or a line that the notebook contains no notes
  yet. After a compaction, the same notices follow the summary.
- `search` lists the lines of the selected notebook's current notes that
  match `pattern`, as `file:line:text`. Each line of `pattern` is an
  extended regular expression, and case is ignored.
- `list` shows each current file once, with its byte count and blob ID.
  `history` lists changes oldest first, with full commit IDs, times, the
  lines added and removed, such as `(+3 -1)`, or `(binary)` for a change
  that Git sees as binary, and commit messages. Give `file` to restrict
  history to one exact filename, including deleted notes.
- `read` accepts `revision`, a full commit ID from the selected notebook's
  history, to retrieve an old version. `diff` requires `file` and `from`,
  with `to` defaulting to `current`. Historical reads and diffs identify
  the resolved commits in their output; offsets count that header too.
  Reuse those IDs for stable pagination. Revisions must belong to the
  selected notebook; Git revision expressions and unrelated commits are
  refused.
- `restore` requires `file` and `revision`. It copies that version's exact
  bytes into a new commit in the caller's notebook, recreating a deleted
  note if necessary, and makes no commit if the note already has those
  bytes. It preserves intervening history and other files. If another
  call changes the notebook during the restore, askai makes the
  restore on the new tip as it makes the other writes, and the restore
  fails if the note changed. With `as`, it writes the version as a new
  note of that name, and its commit message names the source as
  `COMMIT:FILE`. To recover only part of an old note, use `read` with
  `revision`, then `replace` with the combined text.
- `copy` puts a note of the notebook that `notebook` names into the
  caller's own, byte for byte, under the same name or the name that `as`
  gives. Its result and its commit message, such as
  `Copy "plan.md" from the notebook of bro at COMMIT (412 bytes)`, name the
  source member and the commit that it read, and with `as` the source
  file. The text of the note does not pass through the model, so a file
  that is needed exactly, or one that is not text, keeps every byte.
- `read` shows a note that is not UTF-8 text, such as code in an old
  encoding or in a mix of them, as a view that tells every byte: UTF-8
  where it is valid, each other byte and each NUL as `⟦XX⟧`, its value in
  hexadecimal, and a `⟦` of the text as `⟦⟦`, after a line that says so.
  A prompt that has the newest note has the same view. The note itself
  is never converted, so the bytes of a string literal that an old
  protocol needs stay as they are. Reads are paged by `offset`. After a
  turn's `read` or `diff`, a notice such as `[askai: notebook imaami
  (person)]` names the notebook, separately from the contents, without
  changing the pagination offsets.
- The model's `attach`, with Gemini's APIs `generate` and `interactions`,
  gives it a whole note from the selected notebook as a file, where `read`
  shows a large note in parts. `askai` uploads the note as it is if it is UTF-8
  text, else its view, as `text/plain`, 16 MiB at most, to Gemini's Files
  API at the root of Gemini's API in the upstream's URL, the part before
  `/v1beta/`, with the URL's `key` parameter and its header lines, so the
  key goes to no other host. A URL without `/v1beta/` offers no
  `attach`. The file follows the results of the turn: a `fileData` part
  in their message, or a `user_input` step with a `document` after them,
  as neither API shows the model a text file in a result. A note is
  uploaded once while its file lasts: `askai` keeps the files of up to
  128 notes, each for the client whose call attached it, by the note's
  git object id, and gives a file out until 6 hours before it expires
  (Gemini keeps a file 48 hours). A turn that fails after an attach
  makes `askai` forget that client's files. A file counts as one token
  per 4 bytes in the context, and one that does not fit the context, which
  Gemini's model says, is refused; Gemini's "input token count exceeds
  the maximum" makes `askai` begin the conversation again from the
  prompt, as a full context does. The other APIs do not offer `attach`
  yet.

For example, both instances below share one notes session:

```sh
./askai -p llama --background --upstream-tools \
  --agent researcher --tool-name ask_researcher \
  --notes /var/lib/askai/session.git --listen 127.0.0.1:8090
./askai -p google -m MODEL --background --upstream-tools \
  --agent reviewer --tool-name ask_reviewer \
  --notes /var/lib/askai/session.git --listen 127.0.0.1:8091
```

Each calls `{"action":"members"}` to see the other agent and its newest
change, then reads the notebook `reviewer` or `researcher` to follow the
progress of the other agent. Automatic provider transcripts go to a
separate repository, by default
`/var/lib/askai/session.git.transcripts.git`; `--transcripts PATH` changes
that location.

### Moving old notebooks

Earlier versions of `askai` wrote the branches
`refs/heads/notes/PARTICIPANT/WHO/SIDE`. `askai` neither reads nor
rewrites them, and the startup report counts them under `session:` as
`legacy`. The trees of the notes are the same, so one `git update-ref`
moves a notebook with its whole history. Choose the member of each old
branch, and run, for example:

```sh
git -C /git/askai-notes7 update-ref refs/heads/agent/bro/notes \
  refs/heads/notes/bro/172b86b746968586/upstream
git -C /git/askai-notes7 update-ref refs/heads/person/imaami/notes \
  refs/heads/notes/cuh/172b86b746968586/client
```

A moved notebook is readable at once by the name of its member. The
member record appears with the first start of the agent's service, or the
first write or ask call of the person. The committer of an old commit
does not change. The old review records under `refs/review/` stay where
they are, and `askai` does not read them.

A client keeps a file that the user gave it, such as an attachment, with an
`add`, `append` or `replace` that has `content` instead of `text`. The
client's program, not its model, fills `content` in with the file, by the
extension `io.modelcontextprotocol/files` of MCP, an experimental draft:

- `askai` marks the argument `content` of the client's `notes` with
  `x-mcp-file`, and advertises the extension in `server/discover` with
  `maxInlineBytes` 4 MiB, or less if `--max-body` has no room for 4 MiB in
  base64 and 64 KiB of the rest of a request; `maxSize` of `x-mcp-file`
  says the same.
- The value is an RFC 2397 `data:` URI, base64 or percent-encoded, and its
  bytes, decoded, become the note, byte for byte. A note of a file is at
  most `maxInlineBytes`, and the request at most `--max-body` bytes.
- `askai` refuses any other value and never fetches one: an `http` URL, or
  `mcp-file:pending`, the start of an upload out of band, which `askai`
  does not take yet.
- The log shows every `data:` value of an argument only by its size.

A client whose program does not know the extension shows its model the
argument, and the model could write a `data:` URI itself; the description
of the argument tells it not to.

## Background jobs and timers for clients

With `--client-jobs`, `askai` offers clients four more tools: `background`,
`jobs`, `wait` and `timer`. Under `askai`'s own policy, every client gets
them. A role grants them by name, as any other tool:

```json
"roles": {"agent": {"tools": ["ask_ai", "background", "jobs", "wait", "timer"]}}
```

- `background` starts a call of another tool of the client, such as
  `ask_ai`, as a job, and answers at once with the job's id, 32 hexadecimal
  digits. The job runs without the request and without the client's
  connection. A job cannot be a call of `background`, `jobs`, `wait` or
  `timer`. The upstream requests of a job are in the background class of
  the scheduler, so the interactive calls go first.
- `jobs` lists the client's jobs, the newest first, and its timers, with the
  time that each job has run or the time since it ended. An ended job is
  "unread" until a `wait` reports its end, or `jobs` or `tasks/get` returns
  its result. `jobs` returns the result of an ended job, which is the result
  that a direct call of the tool returns. It also cancels a job that runs or
  a timer that has not fired.
- `wait` waits up to 10 minutes until a job ends, of those it names or any,
  or a timer fires, and then lists what happened. Every wait ends at once if
  a timer has fired unread, and removes the timer, so each timer goes to one
  wait only. A wait without ids also ends at once if a job has ended unread,
  and marks the job read, so that no other wait without ids reports it. A
  wait that names a job ends at once if that job has ended, read or not, so
  every wait that names the job reports it. A wait uses no upstream slot.
  On an event stream, it sends keep-alives (`--sse-keepalive`), but a client
  that ends its requests after a time of its own needs a shorter wait.
- `timer` sets a timer of 1 second to an hour, with a note of up to 200
  bytes.

The jobs and the timers belong to an owner: the Unix user of a client of a
Unix socket, the identity of a client over TCP, or else the client's
address (IPv4, or the /64 of IPv6). Every connection of the owner, on any
event loop, can list, wait for, read and cancel the same jobs and timers.
Agents with the same identity, the same Unix user or the same address share
their jobs and timers. A wait without ids can report a job of another
agent, and every wait can report a timer of another agent. A wait that
names only the jobs of its agent reports no job of another agent, but it
still reports the timers of the other agents. Only an identity for each
agent separates both the jobs and the timers.

A job is a task of the tasks extension too: `tasks/get` and `tasks/cancel`
accept its id. A call that became a task is in the list of `jobs` as a
`task`, and `wait` and `jobs` accept its id. A call of `background`, `jobs`,
`wait` or `timer` never becomes a task.

An owner runs at most 8 jobs and tasks at once, and `background` refuses a
ninth job. An owner has at most 8 timers, set or fired and unread, and at
most 4 waits at once. `askai` stores the result of a job for up to an hour
after the job ends: it removes ended jobs sooner in the three cases in which
it removes ended tasks sooner (above). One of them is a new job or task of
an owner with 64 jobs and tasks, which makes `askai` remove the owner's
oldest ended one. `askai` removes a fired timer that no wait reports an
hour after it fired. `askai` stores the jobs, their results and the timers
in memory only, so a restart of `askai` deletes them, unlike the
interactions that Gemini's Interactions API stores. Under `askai`'s own
policy, remote clients without a token also get the four tools, with the
limits of their address only; give remote clients tokens. `askai` has room
for the jobs, timers and waits of 1024 owners, and at most 512 of them can
be addresses, so that anonymous clients cannot fill the room of identities
and Unix users.

## Security

`askai` spends your API credits, so by default it listens on 127.0.0.1 only and
refuses browser requests whose `Origin` is neither loopback nor allowed by `-o`,
which defeats DNS-rebinding attacks. Before listening anywhere else, set
`ASKAI_TOKEN`, or name identities in a configuration file (below); clients must
then send `Authorization: Bearer <token>`, which the web UI can add as a custom
header. A Unix socket (`--listen unix:PATH`) knows its clients' users without
a token; its file's mode follows `askai`'s umask, and `askai` removes it as it
stops. A client that sends a bearer token on a Unix socket gets the status 400,
so that no user can act as another with a token, except for `ASKAI_TOKEN` under
`askai`'s own policy. `askai` also refuses, with 403 as it accepts the
connection, a client whose uid one user can give to processes of another: a
subordinate uid of `/etc/subuid`, which the user that the range is delegated to
can take in a user namespace, uid 4294967295, and the kernel's overflow uid
(usually 65534, so a process that runs as `nobody` is refused). It reads
`/etc/subuid` at start and on each reload, and the startup report lists the
refused uids. With `--git`, whoever may call the tool may read the repositories
listed, and with `--upstream-tools`, so may the model that answers them. With
`--code`, whoever may call the tool can read the committed C code of the
listed repositories through clang. `askai-code` runs under Landlock and
seccomp unless `--code-unconfined` is set (see Asking clang about C code). An
session's notes are readable by everyone allowed to call `notes`, including
anonymous clients if the policy grants it. Each member writes only its own
notebook, and a client without a member name writes none. Every member of
the session can read the evidence that the findings store: tool output and
lines of repositories as the model got them, also a member whose role does
not grant the tool that made them. Tokens cross the
network in the clear but over TLS (below), or a proxy that ends it. A plugin
is code that runs inside `askai` with all of `askai`'s rights. Whoever may
change the configuration file decides which files `askai` loads as plugins.
Whoever may change one of those files, or a directory above it, decides
which code runs. `askai` refuses a configuration file or a plugin's file
that others than root and `askai`'s user may change, or that is in a
directory that they may change (below).

`askai` makes itself non-dumpable as it starts (`PR_SET_DUMPABLE`), before it
reads any token or key. Other processes of its user cannot read its memory or
its environment in `/proc` or attach to it with `ptrace`, and it leaves no core
dump. A debugger must run as root. The programs that `askai` runs, such as
`git` and `man`, are dumpable again after they start.

## TLS

`--listen tls:[host:]port` serves HTTPS on that address: TLS 1.3 only, with
ALPN `http/1.1` (a client that offers ALPN must offer that), its key exchange
X25519MLKEM768 first, else X25519 or P-256. `--tls-cert FILE` names the
certificate chain, in PEM: the server's certificate, then those it needs to
be trusted by, which go to clients as they are; `--tls-key FILE` its private
key, in PEM and unencrypted: ECDSA on P-256, P-384 or P-521, or Ed25519
(which browsers do not take), and RSA only with `--tls-allow-rsa`, as its
handshakes take three times the CPU. Both files must pass the checks a
configuration file does (below), and none but its owner and its group may
read the key's. certbot's `live` files are symbolic
links: have a deploy hook copy them.

```sh
askai -u http://127.0.0.1:8081/v1/chat/completions \
      -l tls:0.0.0.0:8443 --tls-cert /etc/askai/chain.pem --tls-key /etc/askai/key.pem
```

A client loop shakes hands with each client, a flight a step among its bulk
work, so that handshakes wait behind cheap requests rather than hold them
up; the client must be done within `--head-timeout` of its first byte, and
one that sends nothing is closed after `--idle-timeout`, as over plain TCP.
A client that sends plain HTTP, or that offers only TLS 1.2 or another
protocol than HTTP/1.1, is closed with no answer, as is one over the
per-address cap. Each connection gets one stateless ticket, and no session
is cached. SIGHUP reads the certificate and key again, with or without
`--config`: new connections shake hands with the new ones, those open go on
with the old. Files that do not load, or a key that is not the
certificate's, are logged, and the last stay. Who a TLS client is follows
the same policy as over TCP, by token or by where it connects from.
`GET /metrics` counts each client loop's handshakes, done and failed, and
times their flights, mostly CPU (`askai_tls_handshakes_total`,
`askai_tls_flight_seconds`).

## Configuration file, identities and roles

`--config FILE` (`-c`) reads options from a JSON object before the command
line. The file must be strict JSON, so that tools such as `jq` can read it:
comments and trailing commas are errors. `agent` and `transcripts`
are top-level members. The other options but those of the
built-in tools are members of four objects, and each member is named as a
long option is, but `--service-name` is `service.name`:

- `upstream` has the options of the upstream AI, the tools of the upstream
  and the ask tool: the options under "Upstream AI" in `askai --help`, such
  as `url`, `upstream-tools`, `max-turns` and `tool-name`.
- `service` has the name of the service and the options of the MCP server
  and the limits on clients, such as `name`, `listen`, `tls-cert`,
  `allow-origin` and `max-body`.
- `testing` has the test aids, such as `test-fifo`.
- `debugging` has `verbose` and `watchdog`.

The options of the built-in tools are members of the objects of these tools
in the top-level member `tools`, with the names that `askai --help` shows
in parentheses:

- `--man` is `tools.man.enabled`.
- `--git` is `tools.git.repositories`.
- `--git-timeout` is `tools.git.timeout`.
- `--code` is `tools.code.compile-commands`.
- `--code-unconfined` is `tools.code.unconfined`.
- `--notes` is `tools.notes.directory`.
- `--client-jobs` is `tools.jobs.clients`.

An option other than `agent` or `transcripts` at the top level of the
file, or an option in another object than its own,
stops `askai` with a message such as "url must be in upstream", or "man must
be tools.man.enabled" for an option of a built-in tool. The values have
these forms:

- An option with an argument takes a string or a number. A number must be an
  integer, except for `temperature`: `"slots": 2.0` is refused.
- `extra` takes an object, or a string of JSON text.
- An option without an argument takes `true`. `false` changes nothing, and
  the command line cannot turn off an option that the file sets.
- A repeatable option (`header`, `allow-origin`, `listen`,
  `tools.git.repositories` and `tools.code.compile-commands`) is one value
  or an array of values.

The command line overrides the file. It adds to the lists of headers,
allowed origins, git repositories and compile commands of the file, but any
`-l` replaces the `listen` list of the file, and `-x` replaces its `extra`.
Besides the four objects, the top level of the file can have six members:

- `roles`, `identities`, `unix-groups` and `anonymous` are the policy below.
- `plugins` lists the plugins that `askai` loads, each by its path or as an
  object with its path and its options (see Plugins).
- `tools` can contain the objects of the built-in tools, and allows or
  denies tools other than `ask_ai`, to clients and to the upstream (below).

Any other member stops `askai`, and so do `config` and `help`. If two
members of one object have the same name, the last one counts. Only the
last `--config` is read.
The file must be a regular file of at most 1 MiB, not a symbolic link, and
only root or `askai`'s user may change it or any directory above it (others
may add to a sticky one, such as `/tmp`).
[`doc/configuration.md`](doc/configuration.md) is the reference of every
member: its values, its default and what it does.
[`doc/askai.example.json`](doc/askai.example.json) has every member: each
option with an example value, a plugin, `tools` and a policy.
`doc/configuration.md` lists the values to change before you use it.

Four more members define who may connect, and what each client may do:

```json
{
  "upstream": {
    "url": "http://127.0.0.1:8081/v1/chat/completions",
    "upstream-tools": true
  },
  "service": {
    "listen": ["127.0.0.1:8090", "unix:/run/askai/askai.sock"]
  },
  "tools": { "man": { "enabled": true } },
  "roles": {
    "admin":  { "tools": ["ask_ai", "man"], "metrics": true },
    "user":   { "tools": ["ask_ai"], "upstream": ["man"] },
    "reader": { "tools": ["man"] }
  },
  "identities": {
    "alice": { "role": "admin",
               "tokens": ["sha256:9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08"] },
    "ci":    { "role": "user", "tokens": ["env:ASKAI_CI_TOKEN"] }
  },
  "unix-groups": { "developers": "user" },
  "anonymous": { "loopback": "reader", "remote": null, "unix": null }
}
```

- A **role** lists in `tools` the tools that its clients may list and call,
  as clients see them: the tool that `--tool-name` names, `man`, `git`,
  `code`, `notes`, `test_task`, the tools of the plugins, and with
  `--client-jobs` `background`, `jobs`, `wait` and `timer`. `metrics` sets
  whether the clients may read `GET /metrics`. A role name has 1 to 64
  characters from `A-Z`, `a-z`, `0-9`, `_`, `.`, `@` and `-`. A role without
  `tools` grants no tool, and `metrics` is `false` unless the role sets it. A
  name in `tools` that `askai` does not offer in this run, such as `man` without
  `--man`, stops `askai`, and the error lists the tools that it offers.
  `tools/list` shows a client the tools of its role only, and a call of
  another tool is an error, "Tool not available: NAME".
  With `--upstream-tools`, the member `upstream` lists the tools that the
  upstream may call in the turns of the `ask_ai` calls of the role. These
  can differ from the tools that the clients of the role may call. In the
  example, a client of the role `user` cannot call `man`, but the upstream
  can call it for the client. Without `upstream`, the upstream may call the
  tools that the role's `tools` lists. `upstream` can name `man`, `git`, `code`,
  `notes` and the plugin tools that may be lent. Without `--upstream-tools`,
  `upstream` must be empty or left out. The turns also get `notes` whenever
  `--notes` is given, `compact`, `jobs`, `wait` and `timer` whenever they get
  any tool, `findings` too with `--notes`, and `background` beside a tool
  that can run in the
  background (`man`, `git`, `code`, or a plugin tool that allows it). `upstream`
  cannot name `compact`, `findings`, `background`, `jobs`, `wait` or
  `timer`, and `tools` cannot name `compact`. With `--notes`, `tools` can
  name `findings`, which lets clients list, show and expand findings. With
  `--client-jobs`, `tools` can name `background`, `jobs`, `wait` and
  `timer`, which are then the client's own tools (see
  Background jobs and timers for clients above).
- An **identity** has a name, with the same rule as a role name, a `role`,
  and `tokens`, which must not be empty. A client proves an identity with a
  bearer token, `Authorization: Bearer TOKEN`. `"sha256:HEX"` is the SHA-256
  digest of the token (`printf %s TOKEN | sha256sum`), so the file does not
  contain the token. `"env:NAME"` reads the token from the environment of
  `askai`. A reload reads the file again, so it can change a `sha256:`
  token, but the environment of a running `askai` does not change, so a new
  `env:` token needs a restart. The same token in two places stops `askai`.
  No Unix user is an identity, and a client of a Unix socket cannot send a
  token. An identity with `"agent": true` is an agent of the session of
  `--notes`, such as an AI client program with a token of its own. Any
  other identity is a person.
- **`unix-groups`** gives the users of a Unix socket the role of the first
  of their groups it names. Their groups are the effective gid
  (`SO_PEERCRED`) and the supplementary groups (`SO_PEERGROUPS`) that they
  had when they connected. After `newgrp`, the effective gid can differ
  from the primary group. A group whose gid is in a range of `/etc/subgid`,
  or is the kernel's overflow gid, stops `askai`, because a process in a user
  namespace can have such a gid without being a member of the group.
- **`anonymous`** names the role of a client that shows no token: `loopback`,
  one on loopback that names it as `localhost`, `127.0.0.1` or `[::1]`;
  `remote`, any other over TCP; `unix`, a user of a Unix socket that no
  group names. `null`, as leaving it out, refuses them.

A token that no identity has is refused with 401, on the request's head before
its body, as is a client without one that no role is named for. Under a policy
from the file, and under `askai`'s own policy without `ASKAI_TOKEN`, any token
on a Unix socket is refused with 400. An identity's calls are one principal of
the scheduler's, whichever of its tokens they show, and each user of a Unix
socket is one; anonymous TCP clients are their sessions or addresses, as before.
The principal of an identity or of a Unix user is also the owner of its tasks,
jobs and timers. `askai` derives it with HMAC-SHA-256 from the
name of the identity or the uid of the user, keyed with the file
`askai-journal/principal.key` of the transcripts repository, so it stays the
same across restarts and in every instance that uses that repository (see
"Principals and their key" in doc/configuration.md). Stored jobs from
versions of `askai` without the key are no longer anyone's own. Group names
are looked up as the file is read, which with a directory service such as
SSSD may wait on it. Without those four members, `askai`'s own policy
applies. With `ASKAI_TOKEN` set, clients must send it and may do everything.
Else clients on loopback that name it so, or on a Unix socket, may do
everything, and other clients may call the tools. Any of the four members,
also with the value `null`, turns `askai`'s own policy off, and `askai` then
ignores `ASKAI_TOKEN` and logs that. So a file with `roles` alone refuses
every client.

The member `tools` allows or denies the tools other than `ask_ai`, to
clients and to the upstream. Its value has one of these forms, which turn
no tool on:

- `true` allows all tools, and `false` none.
- A list, or an object `{"allow": [...]}`, allows the tools of the list.
- An object `{"deny": [...]}` allows all tools but those of the list.
- An object with the members `client` and `upstream`, each of them one of
  the forms above, sets clients and the upstream apart. A missing member
  allows all.

As an object, `tools` can also contain the objects `man`, `git`, `code`,
`notes` and `jobs` with the options of the built-in tools, which turn these
tools on as the options on the command line do. These objects are members of
`tools` itself, not of `client` or `upstream`. An object without `allow`,
`deny`, `client` and `upstream` allows all tools.

```json
{
  "upstream": {
    "url": "http://127.0.0.1:8081/v1/chat/completions",
    "upstream-tools": true
  },
  "tools": {
    "client": { "deny": ["git"] },
    "upstream": ["man", "notes", "compact"],
    "man": { "enabled": true },
    "git": { "repositories": ["/srv/git/proj"] },
    "notes": { "directory": "/var/lib/askai/notes" }
  }
}
```

With this file, `askai` offers `man`, `git`, `notes` and `findings`. No
client gets `git`, whatever its role grants, and a role may name `git`
without an error. The turns of an `ask_ai` call get at most `man`, `notes` and
`compact`, and never `git`, `findings`, `background`, `jobs`, `wait` or
`timer`. Clients also get `findings`, which `notes` turns on. A
name in `tools` must be a tool that `askai` offers in this run. The
startup log shows a tool that `tools` denies to clients as `off (tools)`.
A change of `tools` needs a restart, also a change of an object of a
built-in tool. [`doc/configuration.md`](doc/configuration.md#tools) has
the details.

With `--config`, or a TLS listener, SIGHUP reloads who may connect and what
they may do, and the TLS certificate and key: a blocker thread reads the
command line, the file and those again, and the new policy takes over for
the requests that begin from then on, while those under way finish as they
began. A file that is wrong is logged, and the last policy stays; the other
options take a restart, as the log says when they changed. Without either,
SIGHUP stops `askai`, as SIGTERM does.

## Plugins

A plugin is a shared object that adds tools to `askai`. The configuration
file lists the plugins in the member `plugins`, an array. An element of the
array is the absolute path of a plugin, or an object whose member `path` is
that path and whose member `options` has the plugin's options. There is no
command-line option for it.

```json
{
  "upstream": { "url": "http://127.0.0.1:8081/v1/chat/completions" },
  "plugins": [
    "/usr/libexec/askai/one.so",
    { "path": "/usr/libexec/askai/test_plugin.so",
      "options": { "greeting": "hi" } }
  ]
}
```

`askai` loads the plugins as it starts, in the order of the array `plugins`,
and never unloads one. A reload loads nothing. A change of `plugins`, also
of the options of a plugin, takes effect when `askai` is restarted. Until
then, each successful reload logs the warning "other changed options need a
restart".

The options of a plugin are an object, which the plugin defines. As
`askai` loads the plugin, it passes the object to the plugin's
`configure()` (`src/plugin.h`), or nullptr if the element has no `options`.
`configure()` checks the options and stores the plugin's configuration.
All calls of the plugin's tools share the configuration, and nothing changes
it. A call gets it with `mcp_slot_config()`. A plugin without
`configure()` has no options. `askai` refuses an element that has another
member, `options` that is not an object, options for a plugin without
`configure()`, and options that `configure()` refuses. A refusal stops
`askai` with exit status 2 and a message with the reason.

In `tools/list`, the tools of the plugins come after `askai`'s own. They are
in the order of the array `plugins`, and the tools of one plugin are in the
order of its manifest. A role names them as it names any other tool. Under
`askai`'s own policy (above), every client that `askai` accepts may call them.
With `--upstream-tools`, the other model may also call a tool of a plugin if
the tool's kind has the flag `TOOL_LENT` (`src/tool.h`) and the caller's role
lets the upstream call the tool.

A plugin can store state in instances. An instance is memory that `askai`
allocates and that the plugin initializes. Each call of a tool of such a
plugin uses one instance of the plugin. Two users never get the same
instance, and only one loop thread uses an instance.

`askai` determines the user of a call in this order. If the client is
connected over a Unix socket, the user is its Unix user, also when it sends
`ASKAI_TOKEN`. If a client over TCP sent a token and the policy has tokens,
the user is the identity that has the token. Under `askai`'s own policy, all
clients over TCP that send `ASKAI_TOKEN` are one user. Every other client is
anonymous. A tool call that the other model makes in a turn of a user's
`ask_ai` call is a call of that user.

A user whose connections are on several loop threads has one instance on each
of them, and these instances share nothing. Each call of an anonymous client
gets a new instance, and `askai` destroys that instance after the call. On
one loop thread, the calls of a user to the tools of one plugin get the same
instance, with these exceptions:

- A loop thread has at most 256 instances that later calls of their users can
  get. When a call of a user needs one more, `askai` destroys the one that
  has been unused for the longest time. If calls use all 256 instances, the
  call fails with the text "Too many tool calls are under way. Try again in a
  few seconds."
- Under memory pressure, `askai` destroys instances that no call uses.
- After a successful reload, the calls of new requests get new instances. A
  call that started before the reload uses its instance until the call ends.
  A call that starts after the reload in an older request, for example a tool
  call in a turn of an `ask_ai` call, gets a new instance for that call alone.

A plugin may not use a socket or libcurl itself, because a plugin starts no
thread and never blocks. A call of a plugin's tool sends an HTTP request
through `askai` with `mcp_slot_fetch()` instead (`src/mcp.h`, `src/web.h`).
The call waits while `askai` sends the request on its home thread. When the
response has come, or the request has failed, `askai` runs the `step()` of
the tool's kind, which reads the result with `mcp_slot_fetched()`. The
result has the status, the `Content-Type`, the body, the seconds of
`Retry-After`, the age of the response in seconds, and an error number and
text if the request failed. A request is a GET, or a POST with a body:

- The URL begins with `http://` or `https://` and has at most 4096 bytes of
  printable ASCII without spaces. It has no user name and no password.
- The body of a POST has at most 4 MiB. `askai` sends it with
  `Content-Type: application/json`.
- The value of `Accept`, if the request has one, has 1 to 256 bytes of
  printable ASCII.
- `max` is the largest length of the response body, at most 16 MiB. A
  longer body fails the request with `EFBIG`. `askai` asks for a
  compressed body with `Accept-Encoding`, and `max` is the length after
  `askai` has decoded it.
- `timeout` is the number of seconds that the whole request may take, 1 to
  300. A request that takes longer fails with `ETIMEDOUT`. So does a
  request whose connection to the server is not set up within 10 seconds.

`askai` sends the header `User-Agent: askai (plugin NAME)`, where NAME is
the name in the plugin's manifest, and it follows no redirect. It sends at
most 64 requests at once, and it opens at most two connections to one host.
The requests of the plugins do not use the connections of the upstream
calls. If a call ends while its request is under way, `askai` cancels the
request, unless the request's `keep` is not 0. With `--verbose`, `askai`
logs one line for each request that it sends, when the request ends.

A request with `keep`, a number of seconds up to 7 days, can use a response
that `askai` stored for an equal request of any user, call or loop. Two
requests are equal if they come from the same plugin and have the same
method, URL, `Accept`, `max` and body. `askai` uses a stored response only
if it received the response less than `keep` seconds ago, and the age in
the result is the number of seconds since then. If an equal request with
`keep` and the same `timeout` is under way, the call waits for the response
to that request, and `askai` does not send a second request. `askai` stores
a response only if the request has `keep`, the status is from 200 to 299,
and the whole body came. The keep of a stored response is the largest
`keep` of the requests that waited for it. A stored response stays after
its keep has passed, until `askai` needs the memory. The stored responses
use at most 64 MiB, together with their keys and `askai`'s records of them,
and a response that would use more than 16 MiB is not stored. To make room
for a new response, `askai` frees first the responses whose keep has passed,
and then the responses whose keep passes next. Under high memory pressure
it frees the responses whose keep has passed, and under critical memory
pressure it frees all of them.

`askai` reads the manifest from each file before it loads the file. It
refuses a plugin that breaks one of these rules:

- Only root and `askai`'s user may change the file and the directories above
  it, as for the configuration file. The path must be the file's real path,
  without a symbolic link, a `.` or `..` component or a double slash.
- The file must contain a manifest, which `PLUGIN_MANIFEST()` of
  `src/plugin.h` puts into every plugin.
- The plugin interface version and the sizes and offsets in the manifest must
  equal those of `askai`. They differ when the plugin was built from other
  sources or with a compiler or options that change the layout of the shared
  structures.
- The file must have no thread-local storage, no constructor or destructor,
  and no data that is still writable when the plugin's code runs, because a
  plugin has its state in instances. The link flags `-nostartfiles
  -Wl,-z,relro,-z,now` give such a file; a plugin sets them in its
  `src/NAME.so.mk`, as `src/test_plugin.so.mk` does. `askai` that is built
  with `-fsanitize=address`, `-fsanitize=thread` or `-fsanitize=undefined`
  does not check this.
- No tool of the plugin may have the name of a tool that `askai` already has,
  its own or an earlier plugin's.
- Each kind of tool of the plugin may have only the flags that `src/tool.h`
  defines for plugins (`TOOL_FLAGS`), and the definition of each tool must
  have the tool's name that the manifest gives.
- The plugin must not call a function of `askai` that `askai` does not export
  (`src/export.h`). `dlopen()` fails for such a plugin. `askai` does not
  check which functions of other libraries the plugin calls.

A refused plugin stops `askai` as it starts, with exit status 2 and a message
that names the file and the reason.

A plugin is built by `askai`'s make rules: `src/NAME.so.mk` lists its sources,
as `src/test_plugin.so.mk` does for the test plugin, and `make NAME.so` builds
it. `src/plugin.h` documents what a plugin provides and what it must not do,
and `src/test_plugin.c` is the smallest example.

### The godbolt plugin

`src/godbolt.c` is a plugin for Compiler Explorer, the service at
https://godbolt.org or another installation of it. `make godbolt.so` builds
it. It has these tools:

- `godbolt_catalog` lists the languages of the service with the default
  compiler of each, the compilers of one language, or the numbers of
  compilers of each kind, family or architecture. Its filters select
  compilers by id, kind, family tag, architecture, and words in their ids,
  names, kinds, versions, architectures, release tracks and family tags.
- `godbolt_compile` compiles source code once with each of up to 16
  compilers and each of up to 8 argument strings, and at most 32 times in
  all. The output has a record for each compilation, with the exit code,
  the assembly, and the output and the messages of the compiler. With
  `run` true, Compiler Explorer also runs the program on its servers,
  without arguments and with empty standard input. The record then
  contains a line `program: exit code N`, `program: timed out` or
  `program: not run`, the standard output and error of the program, and
  the messages of the build if the program did not run.
- `godbolt_preprocess` preprocesses source code in the same way, and its
  records have the preprocessed source.
- `godbolt_macros` outputs the predefined macros of each compiler, and of
  the source if the call has one. MSVC cannot output its predefined
  macros, so the plugin sends no request for an MSVC compiler.
- `godbolt_targets` preprocesses a fixed probe, and its records have facts
  about the target that follow from the predefined macros. The facts are
  the architecture, the operating system, the data model, the width of a
  pointer, the byte order, the C library and the versions. The line of each
  fact lists the macros that the fact comes from. With `headers`, the probe
  also includes a few target headers, such as `limits.h` and `features.h`.
  Without `headers`, the C library is `unknown without headers`, because
  only a header of the library defines its macro, such as `__GLIBC__`.
  The probe and its parser come from godbolt-c, under the MIT license
  (`src/godbolt_probe.c`).

The other tools read the list of compilers of the language before they
compile. A compiler id that is not in the list fails the call before it
sends a compilation, and the text of the failure contains the call of
`godbolt_catalog` that lists the ids. The list also contains the kind of
each compiler. The arguments for an MSVC or clang-cl compiler begin with
the flags of its driver, such as `/TC /EP`, and the text is in the
standard output of the compiler. The arguments for every other compiler
begin with the flags of GCC, such as `-E`.

Without the argument `compilers`, the other tools compile with the
default compiler of the language, which `godbolt_catalog` lists with the
action `languages`. A language without a default compiler fails the
call.

The plugin has two options. `url` is the base URL of the service, and its
default is `https://godbolt.org`. `share` is `short`, `state` or `off`, and
its default is `short`.

```json
{ "path": "/usr/libexec/askai/godbolt.so",
  "options": { "url": "https://godbolt.org", "share": "short" } }
```

A call sends the source code and the argument strings to that service. A
call with the argument `offset` gets the output from that byte on, as a call
of `man` does. A compilation that fails, for example with an HTTP error, is
a record with the reason, and the call fails only if no compilation returns
a result.

The plugin removes the CSI escape sequences of up to 32 bytes from the
texts of the records, such as the color codes that GCC and Clang write on
Compiler Explorer.

A response to a compilation has at most 4 MiB. The output of a call ends
at 14 MiB with a note, and the plugin sends no compilation after that. A
list of compilers has at most 4 MiB, and an element of a list at most
4 KiB. The catalog leaves out a compiler with more than 64 family tags, or
with more than 128 bytes in its kind, its architecture or a family tag, and
a line of the output counts the elements that it left out. The counts of
`godbolt_catalog` have at most 256 different keys. A name or a value in
the output of the probe of `godbolt_targets` has at most 256 bytes.

The plugin sends a request at most 3 times. It sends the request again
after the status 429, 502 or 503. It sends a request for a list or for a
short link again also after the status 504 and after a transfer that fails
in another way, and a request for a list also after a timeout. Before the
second attempt it waits 1 s, and before the third 2 s, or the seconds of
`Retry-After` if they are more. If `Retry-After` is more than 30 s, the
request fails at once.

A call of `godbolt_compile`, `godbolt_preprocess` or `godbolt_macros` with
the argument `share` true has a link on the first line of its output. The
link opens the same source, compilers and arguments on Compiler Explorer.
The option `share` selects the kind of link:

- `short`: Compiler Explorer stores the source, the compilers and the
  arguments permanently, and the link has a short id, such as
  `https://godbolt.org/z/Ab12cd34e`. Anyone with the link can read the
  source. Compiler Explorer returns the same link for the same state.
- `state`: the link has the state in base64, and Compiler Explorer stores
  nothing. If the link would be longer than 8192 bytes, the line has the
  reason instead of a link.
- `off`: the tools have no argument `share`, and a call with it fails.

Unless links are off, the description of each of these tools has the
sentence "Set share only if the user asks for a link." With `short`, their
`readOnlyHint` is false. If a link fails, the line has the reason, and the
compilations go on. A call that sends no compilation, such as a call of
`godbolt_macros` with only MSVC compilers, gets no link.

The `keep` of a request for the list of languages is a day, and the `keep`
of a request for a list of compilers is an hour, so a repeated call of
`godbolt_catalog` usually sends no request. A call that compiles, without
`offset`, sends each compilation, unless `askai` received the response to
the same compilation less than a second before. A call with `offset` gets
the responses that `askai` received for the same call without `offset` in
the last 5 minutes, if `askai` still stores them, and then both calls have
the same output. `askai` does not store a failed response, so a call with
`offset` sends a failed request again, and the record of that compilation
can differ. The `keep` of a request for a short link is a day.

## Limits

`askai` bounds what each client may send and hold, so that slow, oversized or
abusive clients cannot tie up its event loop, its memory or its descriptors.
Every limit is an option (see below); a breach gets the HTTP status a client
can act on:

| Limit | Default | Breach |
| ----- | ------- | ------ |
| request line and headers | 16 KiB, 64 fields, sent within 10 s | 431, 408 |
| request body | 8 MiB, averaging at least 4 KiB/s after 5 s | 413, 408 |
| JSON in a body | 32 levels deep, 1024 values, 1024 members in an object, numbers of 1 KiB | 400 |
| bodies held for all clients | 256 MiB | 503 |
| bodies held for one client address | a quarter of those | 503 |
| connections per client address (IPv4, or IPv6 /64) | 256 | 503 |
| idle keep-alive | 30 s | close |
| pending output | 256 KiB holds requests back, 8 MiB unless one response is all of it | close |
| output stalled | 30 s | close |

The limits hold for the whole process, whichever event loop serves a client.
Bytes trickling in or out never extend a deadline. When `askai` ends a
connection, it half-closes it and discards what the client still sends for up
to 2 s, so that a client that sent too much still reads the error. With
`ASKAI_TOKEN` set, a request without the token is refused before its body.
Raise `--max-body` (and `--body-budget`) to pass large documents to the other
model. Bodies and answers are parsed and freed, prompts and answers passed
on, and replies sent, a slice at a time, so a large one hardly holds up other
clients: only copying its longest string takes a single step, of about 0.4 ms
per MiB, as does copying an answer's text (answers may take up to 16 MiB).
Raising `--max-values` lets a body hold more values, each of which json-c
holds in up to 0.7 KiB while the request runs.

`askai` keeps the memory its work has used for the work to come: buffers go
back to a stash of the event loop and come out of it again, up to twice the
most they held at once, and finished requests are kept for reuse too, so a
steady load allocates none. It gives memory back to the system only under
memory pressure: resident memory over `--memory-high` (by default a sixteenth
of RAM, or of the lowest cgroup limit, at least 256 MiB), RAM or a cgroup's
limit nearly used up (file cache the kernel can reclaim aside), or the
kernel's pressure stall information. It then frees what went unused for
10 s or more, or under critical pressure all it keeps, a slice at a time, and
the allocator returns what is free to the system in one call that takes about
55 µs per MB.

Sending `askai` SIGUSR1 prints statistics of its event loops since the
previous SIGUSR1, each loop's in lines that name it (`loop1` to `loopN`, and
`home`): how long their turns and steps took, how late their timers fired,
and the most work of each kind a step did against its bound.

## Running

`askai` serves clients on event loop threads, by default one fewer than the
CPUs it may use, as its affinity and its cgroup's CPU quota allow (`--loops`),
and asks the other model from another, home. Home accepts the clients and
places each on the lighter of two client loops, by the connections they hold
and how busy they are; the client stays with that loop. A tool call hands its
upstream request to home as a job, which home runs and hands back. Home makes
as many upstream requests at once as the upstream has slots (`--slots`; by
default what `llama-server`'s `/props` says, which `askai` asks of `-p llama`
and of a server no provider names, unless its API is `generate` or
`interactions`; else 4). The other calls wait, each in its principal's queue, its client's
identity's, else its MCP session's or, without one, its client address's,
and the principals take
turns, a call each, so that one flooding client leaves the others their share.
At most `--max-calls` (256) are under way, and eight queued for one principal
(a call that a free slot takes at once waits in no queue); a tool call past
those is answered at once with a tool error that says to try again. A client that goes while its call waits or
runs cancels it, upstream too, as does a `notifications/cancelled` for it from
the same caller, which can come on any connection. The caller is the user of
a Unix socket's client. For a TCP client, it is the client's identity if it
has one, and otherwise the client's address (IPv4, or IPv6 /64). The notification must also carry
the same `Mcp-Session-Id` field as the call, or none if the call has none.
After five upstream failures in a row (connection
errors, timeouts, HTTP 5xx and 429), or an answer's `Retry-After`, tool calls
are refused at once, saying when to try again: for a second, or as long as
`Retry-After` asked. Then one call probes the upstream, and the wait doubles,
up to a minute, until one goes through: as soon as its answer begins, the
others go too.
The main thread minds signals, the process's memory and descriptors (and the
kernel's pressure stall information where there is some), and a watchdog that
logs a loop turn longer than 20 ms (`--watchdog`). A logger thread alone
writes stderr: a reader of stderr that stops reading holds up no client, as
lines that do not fit in its buffers are dropped and counted. Two more
threads look up the upstream host, so libcurl starts no threads of its own.

`GET /metrics` returns metrics in Prometheus's text format: each loop's turn
and step times, timer lateness and inbox delay (`loop="1"` to `loop="N"`, and
`loop="home"`), each client loop's request times by MCP method and its
connections, home's jobs, its queue, slots and breaker, streamed answers'
chunks, upstream wait and run times, host lookups, log lines dropped,
watchdog reports, memory and descriptors, memory pressure and its inputs, and
what the loops keep for reuse and gave back.
It answers the clients whose role may read it: without a configuration file
that says otherwise, those on loopback that name it as `localhost`,
`127.0.0.1` or `[::1]` (or send no `Host`), or on a Unix socket, and with
`ASKAI_TOKEN` set, those that send the token.

The first SIGINT or SIGTERM, or SIGHUP without `--config` or TLS, makes `askai` drain: it stops accepting
clients, closes idle connections, answers tool calls that wait for the other
model with an error (on an event stream, as its last event), finishes other
requests, and exits 0, all within a grace period of 5 s (`--grace`). A second
signal, the grace period running out, or an event loop stuck in one turn for
a second ends it at once, with exit status 3. A signal that `askai` started
with ignored, as under `nohup` or in a shell's background job, stays ignored.
Invalid options or environment exit 2, and a failure to start or to serve 1.

## Log

`askai` writes its log to stderr, one line for each event. As it starts,
and before the lines with its MCP endpoints, it logs what it runs as lists
with one item a line:

- `upstream:` the provider, API, model, context size and URL. The URL is
  logged without a user, a password and a query, which can have secrets.
- `built-in tools for clients:` each tool, `on` or `off`, and for a tool
  that is off, the option that turns it on, or `tools` if the member
  `tools` of the configuration file denies it to clients. The test aid
  `test_task` is listed only when it is on.
- `git repositories:`, when that tool is on.
- `plugin NAME:` for each plugin that `askai` loaded: its file, whether the
  configuration gives it options, and its tools. A tool that the member
  `tools` denies to clients has `off (tools)` after its name.
- `upstream tools: off (--upstream-tools)` without `--upstream-tools`.
- the policy. `askai`'s own policy is listed with whether `ASKAI_TOKEN` is
  set and what each kind of client may do. The policy of a configuration
  file is listed as each role with its tools and `metrics`; each identity
  with its role, and `agent` for an agent; each Unix group with its role;
  and the role of each kind of anonymous client. With `--upstream-tools`,
  the policy also lists the tools that the upstream may call in the turns of
  an `ask_ai` call, and none for a role without `ask_ai`.
- a warning with the tools that are on and that `tools` allows clients,
  but that no role grants or lends to the upstream, because nothing can
  call them.
- `session:` with `--notes`: the repository, the ID and the name of the
  session, the agent and the service. `session members:` lists each member
  that the records of the session name as `askai` starts, with its kind
  and its logins, such as `uid 1000`. Without `--notes` and with
  a transcript repository, `service:` names the service instead.

A reload logs the policy again. `askai` does not log client tokens, its
API key or the headers that it sends. It logs the error text of an upstream
and the request URL of a plugin as they arrive, and these can contain a
secret. In this configuration file, no role lists a tool of the godbolt
plugin:

```json
{
  "agent": "bro",
  "upstream": {
    "url": "http://127.0.0.1:8081/v1/chat/completions",
    "tool-name": "bro",
    "upstream-tools": true
  },
  "tools": {
    "man": { "enabled": true },
    "git": { "repositories": "/srv/git/proj" },
    "notes": { "directory": "/var/lib/askai/notes" }
  },
  "plugins": [{ "path": "/usr/libexec/askai/godbolt.so",
                "options": { "share": "off" } }],
  "roles": {
    "admin": { "tools": ["bro", "man", "git", "notes"], "metrics": true },
    "user":  { "tools": ["bro", "notes", "git", "man"] }
  },
  "identities": { "alice": { "role": "admin", "tokens": ["env:ALICE_TOKEN"] } },
  "anonymous": { "loopback": "user" }
}
```

`askai` logs these lines as it starts with it:

```text
upstream:
  provider  none
  API       chat
  model     none
  context   from llama-server
  URL       http://127.0.0.1:8081/v1/chat/completions
  files     off (the API takes none)
built-in tools for clients:
  bro         on
  man         on
  git         on
  notes       on
  findings    on
  background  off (--client-jobs)
  jobs        off (--client-jobs)
  wait        off (--client-jobs)
  timer       off (--client-jobs)
git repositories:
  proj  /srv/git/proj
plugin godbolt:
  file     /usr/libexec/askai/godbolt.so
  options  given
  tools    godbolt_catalog
           godbolt_compile
           godbolt_preprocess
           godbolt_macros
           godbolt_targets
policy: from the configuration file
role admin:
  tools           bro
                  man
                  git
                  notes
  upstream tools  man
                  git
                  notes
                  findings
                  compact
                  background
                  jobs
                  wait
                  timer
  metrics         yes
role user:
  tools           bro
                  man
                  git
                  notes
  upstream tools  man
                  git
                  notes
                  findings
                  compact
                  background
                  jobs
                  wait
                  timer
  metrics         no
identities:
  alice  role admin
Unix groups: none
anonymous clients:
  loopback  role user
  remote    refused
  unix      refused
warning: no role lists these tools:
  findings
  godbolt_catalog
  godbolt_compile
  godbolt_preprocess
  godbolt_macros
  godbolt_targets
session:
  repository  /var/lib/askai/notes
  id          6f0c2b9a41d8e3f75a2c1b0d9e8f7a6b
  name        notes
  agent       bro
  service     bro
  evidence    every member can read all stored evidence
session members:
  imaami  person  uid 1000
  bro     agent
MCP endpoint http://127.0.0.1:8090/mcp
```

The lines `upstream slots`, `upstream context`, `upstream /props` and
`upstream model`, which come from llama-server's `/props` or from Gemini's
model, can come before or after these lines. As
`askai` stops, it lists the counts of its requests to the upstream.

Without `--verbose`, `askai` logs each tool call of a client that fails,
with its tool, its client, its time and the text of the failure. It also
logs each tool call of a client that it refuses, because the client's role
does not grant the tool or `askai` offers no such tool:

```text
tool call godbolt_compile from anonymous loopback (role user) refused: the role does not grant it
tool call "godbolt" from anonymous loopback (role user) refused: askai offers no such tool
```

The client over TCP is `identity NAME`, or `anonymous KIND (role NAME)`
for a client that shows no token, where KIND is `loopback` or `remote`.
A client on a Unix socket is `uid N (role NAME)`, with its uid as a
number and the role that its group or `anonymous` gives it. Under
`askai`'s own policy, a client over TCP without `ASKAI_TOKEN` is
`anonymous`, a client on a Unix socket is `uid N`, and one that sends
`ASKAI_TOKEN` there is `uid N (identity ASKAI_TOKEN)`. A name that `askai` does not
offer is written as a JSON string.

With `--verbose`, `askai` also logs these lines:

- each tool call of a client as it ends: the tool, the client, how the call
  ended, its time, the size of its output, the task or the job that the call
  ran as, its arguments as JSON on one line and cut at 200 bytes, and the
  text of a failure. The line of a job has the tool and the arguments of
  the call that the job runs.
- each call of `background` by a client, as the job starts: the client, the
  id of the job and the tool that the job calls.
- each ask call as it starts: its number, `LOOP.N`, its client, the size of
  its prompt, its conversation and the start of the prompt as a JSON
  string of at most 100 bytes. The lines of the call's turns begin with
  `ask call LOOP.N turn T`, and the line of the call's end has the number
  too.
- each turn as its answer ends: the HTTP status, the time, the tokens of the
  conversation and the context size, the stop reason, the tool calls and the
  size of the text. An answer that is not streamed, such as the body of an
  HTTP error, has only its status, its time and its size.
- each tool call of a turn, and of a job in the background, with its
  arguments and the text of a failure. A call of a tool that the ask call
  does not lend, or that `askai` does not offer, is logged as refused.
- each compaction, by the upstream's `compact` call or by `askai`, and each
  notice about the context that `askai` adds to a result.
- each web request of a plugin, and each man page that `askai` renders.

The text of a note and the summary of `compact` are logged only by their
sizes. These are the lines of an ask call whose first turn calls two tools:

```text
ask call 1.1 from anonymous loopback (role user): prompt 85 bytes, conversation b73f6f9a14edb9977c9aa00761470f26 (new), "What is in the repository? Use git to look at its log, and keep a plan in your notes."
ask call 1.1 turn 1: answered, HTTP 200 after 2.4 s, 1230 of 8192 tokens (15 %), stop reason "tool_calls", 2 tool calls, 8 bytes of text
ask call 1.1 turn 1: tool call git answered after 0.1 s (14 bytes); arguments {"repo":"proj","command":"log","options":["--oneline"]}
ask call 1.1 turn 1: tool call notes answered after 0.0 s (6 bytes); arguments {"action":"add","file":"plan.md","text":"(210 bytes)"}
ask call 1.1 turn 2: answered, HTTP 200 after 1.3 s, 1512 of 8192 tokens (18 %), stop reason "stop", 0 tool calls, 30 bytes of text
tool call bro from anonymous loopback (role user) answered after 3.9 s (30 bytes); ask call 1.1, 2 turns, 2 tool calls; arguments {"prompt":"What is in the repository? Use git to look at its log, and keep a plan in your notes."}
```

## askai options

```text
  -c, --config FILE         read options from FILE before the command
                            line: a JSON object without comments.  Its
                            objects upstream, service, testing and
                            debugging contain the options of the
                            sections below, named as the long options,
                            but --service-name is service.name.  Its
                            member tools can contain the objects of
                            the built-in tools, whose members the
                            section Built-in tools names.  true sets an
                            option without argument, and an array lists
                            the values of a repeatable option.  The
                            command line overrides the file, but adds
                            to its header, allow-origin, git and code
                            lists.  Only the last --config is read.  See
                            doc/configuration.md and
                            doc/askai.example.json.
  -h, --help                show this help

Session (top-level configuration members):
      --agent NAME          name of the agent of this askai in the
                            session of --notes, a member name of 1 to
                            64 characters from [A-Za-z0-9_.@-] (see
                            doc/configuration.md); required with
                            --notes
      --transcripts PATH    separate git repository for reasoning text,
                            with private job recovery state; default:
                            <notes>.transcripts.git with --notes, else
                            $XDG_STATE_HOME/askai/NAME/transcripts.git
                            with --background, where NAME is
                            --service-name, using $HOME/.local/state if
                            XDG_STATE_HOME is not absolute

Upstream AI ("upstream"):
  -p, --provider NAME       llama (llama-server at 127.0.0.1:9931),
                            openai, anthropic, google, deepinfra or
                            openrouter; the provider sets the defaults
                            of --api, --url and --key-env
      --api NAME            chat (chat completions), responses,
                            messages (Anthropic), generate (Gemini) or
                            interactions (Gemini Interactions)
                            (default: the API of the provider, else
                            chat)
  -u, --url URL             endpoint URL, http:// or https:// (default:
                            the URL of the provider; required without
                            --provider)
  -m, --model NAME          model to ask (required at the URL of a
                            provider other than llama)
  -k, --key-env VAR         environment variable that contains the API
                            key (default: at the URL of a provider, the
                            usual variable of the provider, which llama
                            may leave unset; else ASKAI_API_KEY, if set)
  -H, --header LINE         extra request header, Name: value
                            (repeatable)
  -s, --system TEXT         default system prompt
  -t, --temperature N       sampling temperature, a number from 0
                            (default: none sent)
  -n, --max-tokens N        output token limit (default: none sent, but
                            4096 with the messages API)
  -x, --extra JSON          JSON object that askai merges into each
                            request body, recursively; a null member
                            removes that key
      --background          return ask calls immediately as jobs, and
                            keep their upstream streams alive after
                            clients disconnect; request native background
                            execution with OpenAI Responses and Gemini
                            Interactions (default off)
  -T, --timeout SECONDS     time limit on each upstream call: first on
                            the wait for an upstream slot, then again on
                            the call itself (default 600, 0 = none)
      --chunk-timeout SECONDS
                            longest pause in an answer once it has
                            begun (default 60, 0 = none)
      --slots N             upstream calls at once (default: total_slots
                            from the /props of llama-server, else 4)
      --max-calls N         upstream calls queued or running, at most
                            (default 256), and at most 8 queued for one
                            identity, one Unix user, or else one session
                            or client address; an ask call past those
                            fails at once with an error to try again
      --upstream-tools      offer tools to the upstream in ask calls:
                            man, git and the plugin tools that allow it,
                            as the role of the client allows, and notes
                            with --notes; with any of those also
                            compact, jobs, wait and timer, findings with
                            --notes, and beside a tool that can run in
                            the background also background; an ask call
                            runs the tool calls of each turn and sends
                            the next upstream request until an answer
                            ends the call
      --max-turns N         upstream calls that an ask call makes at
                            most, with --upstream-tools (default 32)
      --context TOKENS      context size of the upstream, used with
                            --upstream-tools: a tool result in a turn is
                            at most an eighth of it; at a quarter and at
                            half, the upstream is reminded to write
                            notes, with --notes; from two thirds on, it
                            is asked to call compact; and when it is
                            full, askai compacts the conversation
                            (default: n_ctx from the /props of
                            llama-server, else unknown; 0 = the default)
      --tool-name NAME      name of the ask tool, 1 to 128 characters
                            from A-Z, a-z, 0-9, _, . and - (default
                            ask_ai)
      --tool-description TEXT
                            description of the ask tool (default: a text
                            built from --model, --upstream-tools and
                            --notes)

MCP server ("service"):
      --service-name NAME   name of this askai in its transcripts,
                            its background jobs and its commits, a
                            name as for --agent (default: --agent, else
                            --tool-name; service.name)
  -l, --listen ADDR         [host:]port, where host is 127.0.0.1 if it
                            is left out; unix:PATH for a Unix socket; or
                            tls:[host:]port for TLS (repeatable;
                            default 127.0.0.1:8090)
      --tls-cert FILE       certificate chain for tls: listeners, in
                            PEM: the certificate of the server first,
                            then the intermediate CA certificates
      --tls-key FILE        private key of that certificate, in PEM,
                            unencrypted: ECDSA on P-256, P-384 or P-521,
                            or Ed25519; others must not be able to read
                            FILE
      --tls-allow-rsa       allow an RSA key too
  -o, --allow-origin URL    allow browser requests whose Origin header
                            is exactly URL, such as https://example.com
                            (repeatable; '*' allows any; loopback
                            Origins are always allowed)
      --loops N             event loops that serve clients (default 0:
                            one fewer than the CPUs askai may use, at
                            least 1, at most 255)
      --grace SECONDS       time to finish the requests under way after
                            SIGINT, SIGTERM or SIGHUP (default 5)
      --memory-high BYTES   resident memory from which memory pressure
                            is high (default: a sixteenth of RAM, or of
                            the lowest cgroup limit, at least 256M)

Limits on clients ("service"; BYTES and RATE may end in K, M or G for
KiB, MiB or GiB; 0 = none where noted):
      --max-head BYTES      request line and headers (default 16K,
                            at most 65535)
      --max-fields N        header fields (default 64)
      --head-timeout SECONDS
                            time limit for a TLS handshake, and for a
                            request line and headers, each from its
                            first byte (default 10, 0 = none)
      --max-body BYTES      request body, at most --body-budget
                            (default 8M)
      --min-body-rate RATE  bytes per second that a request body must
                            average from the end of its headers, once
                            --body-grace has passed (default 4K,
                            0 = none)
      --body-grace SECONDS  time after the headers before
                            --min-body-rate applies (default 5)
      --body-budget BYTES   request bodies in memory for all clients
                            together (default 256M)
      --client-budget BYTES
                            the part of --body-budget for one client
                            address (default: a quarter of it, at least
                            --max-body)
      --max-depth N         JSON nesting in a request (default 32)
      --max-values N        JSON values in a request, keys included
                            (default 1024; json-c uses up to 0.7 KiB for
                            each value while it parses)
      --max-members N       members of one JSON object in a request
                            (default 1024)
      --sse-keepalive SECONDS
                            time between the comment lines that askai
                            writes on an idle event stream (default 15,
                            0 = none)
      --max-client-conns N  connections per client address: an IPv4
                            address, an IPv6 /64, or the user of a Unix
                            socket client (default 256, 0 = none)
      --idle-timeout SECONDS
                            time a connection may stay open with no
                            request under way, before its first request
                            and between requests (default 30, 0 = none)
      --output-pause BYTES  pending output at which no more input is
                            read from a connection, and a response is
                            paused until at most half of this is pending
                            (default 256K, at most --max-output)
      --max-output BYTES    close a connection whose pending output a
                            response would take past this, unless it
                            is all that is pending (default 8M)
      --send-timeout SECONDS
                            longest stall of pending output
                            (default 30, 0 = none)

Built-in tools ("tools", each in the member in parentheses):
      --man                 also offer the tool man, which returns a
                            page of the manual as man(1) renders it, in
                            plain UTF-8 text; askai renders each page
                            once and stores it (tools.man.enabled)
      --git PATH            also offer the tool git: read-only git
                            commands in the repository at PATH, the top
                            of a work tree or a bare repository, named
                            by the last component of PATH less .git
                            (repeatable; tools.git.repositories)
      --git-timeout SECONDS
                            deadline of a git call, at which git gets
                            SIGTERM, and SIGKILL one second later
                            (default 30; tools.git.timeout)
      --code REPO=FILE      also offer the tool code, which runs
                            clang on the committed C code of the
                            git repository REPO, a name or absolute
                            path of --git, with the flags of the
                            compile commands in FILE, relative to
                            REPO; askai runs askai-code for each call
                            (repeatable; tools.code.compile-commands)
      --code-unconfined     run askai-code without Landlock and
                            seccomp, which the tool code needs
                            otherwise (tools.code.unconfined)
      --notes PATH          shared session's git repository, created
                            bare if absent, with the session record and
                            the member record of --agent.  Each agent
                            writes its own branches and reads all notes;
                            offer notes to clients as their role
                            allows, and to the upstream with
                            --upstream-tools (tools.notes.directory)
      --client-jobs         also offer clients the tools background,
                            jobs, wait and timer, as their role allows:
                            background runs a call of another tool as a
                            job, whose result askai stores for up to an
                            hour; timer sets a timer; wait waits for
                            jobs and timers (tools.jobs.clients)

Test aids ("testing"):
      --test-man-recheck SECONDS
                            once the lookup of the file of a page (man
                            -w) is older than this, the next man call
                            for the page repeats the lookup in the
                            background (default 600, 0 = on each call)
      --test-stall MS       SIGUSR2 stops each event loop in one turn
                            for this long
      --test-pressure FILE  memory pressure is at least the level in
                            FILE, 0 (none), 1 (high) or 2 (critical),
                            read each second
      --test-fifo           upstream calls wait in one queue for each
                            class, first in first out, and without the
                            limit of 8 queued calls
      --test-task           also offer the tool test_task: CPU work in
                            250 us slices, run as a task on the client
                            loops; or, given a prompt, an upstream call
                            run as background work

Debugging ("debugging"):
  -v, --verbose             log each tool call of a client as it ends,
                            with its arguments, and each ask call as
                            it starts; each upstream turn, each tool
                            call of a turn or of a background job,
                            each compaction and each notice about the
                            context; each web request of a plugin; and
                            each man page that askai renders
      --watchdog MS         log a turn of an event loop longer than
                            this (default 20, 0 = none)
```

## dstr

Strings are handled with `dstr` ([`src/dstr.h`](src/dstr.h)), a dynamic string
with Short String Optimization. A `struct dstr` is a value, not a handle: a
string that fits in the object (23 bytes on 64-bit targets by default) lives in
place, and a longer one in a heap buffer allocated in 256-byte blocks, which is
wiped before it is freed. An all-zero object is *not* an empty string;
initialize every object with `dstr()` or `DSTR_INIT`.

A `dstr` holds UTF-8 text only; anything in another encoding is converted at
the boundary before it goes in (the library doesn't validate this yet). The
null character U+0000 is valid UTF-8, and since a `dstr` keeps its own length,
its content may contain nulls: `dstr_len()` counts every byte, while
`dstr_get()` read as a C string stops at the first null.

The object size can be chosen per translation unit by defining `DSTR_OBJ_SIZE`
(a multiple of `sizeof (uintptr_t)`, from `3 * sizeof (uintptr_t)` to 128)
before including `dstr.h`. The library functions in `dstr.c` take the object
size at run time, so translation units that use different sizes link against
the same `dstr.c.o`.

The test trials live in [`tests/dstr`](tests/dstr): API contracts, a boundary
sweep and a randomized model check at every legal object size, allocation
failure injection, header hygiene, big-endian and 32-bit targets under
qemu-user, and constant folding of short literals.

```sh
tests/dstr/run.sh                        # every trial, with clang and gcc
tests/dstr/run.sh -l 03 -t gcc -S address,undefined
tests/dstr/run.sh -h                     # all options
```
