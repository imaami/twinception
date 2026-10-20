# Terminal chat through AskAI

`askai-cli` is a standalone chat client for AskAI, adapted from llama.cpp's
terminal interface and using GNU Readline for input. Capable interactive
terminals use a full-screen transcript, status row, and input row;
`--simple-io` keeps plain line input and output. Redirected input or output
selects plain mode automatically. The configured upstream model answers
directly, and AskAI executes its permitted tools and stores its notes and
transcripts. The client contains no model loader, tokenizer, sampler, local
inference engine, or llama-server dependency.

## Build

The client needs a C23 compiler, libcurl with Unix-socket support, json-c,
GNU Readline, utf8proc, and their development packages (`libreadline-dev`
and `libutf8proc-dev` on Debian and Ubuntu). From the repository root:

```sh
make askai-cli
build/askai-cli --help
```

`make askai-cli` builds only the client. `make` also builds AskAI and the
other programs. Run the resulting `build/askai-cli` executable directly.

The client links libcurl, json-c, GNU Readline, utf8proc, and the C math
library. It does not link libllama, ggml, or any inference backend.

## Connect

Start AskAI with the provider, model, and tools in your existing configuration:

```sh
build/askai --config /path/to/askai.json --listen unix:/path/to/askai.sock
build/askai-cli --socket /path/to/askai.sock
```

The parent directory of the socket must already exist. The client connects
using MCP over HTTP through that Unix socket. It never opens a TCP connection
to AskAI or connects to the upstream provider itself. AskAI controls upstream
credentials, model options, generation limits, tool access, and persistence.

The client authenticates through Unix peer credentials. The socket's
filesystem permissions and AskAI's Unix-user policy determine who can
connect. See [identities and roles](../README.md#configuration-file-identities-and-roles)
for the server's Unix-user and Unix-group configuration.

Instead of repeating the socket path:

```sh
export ASKAI_SOCKET=/path/to/askai.sock
askai-cli
```

The client discovers the server's ask tool. Use `--tool NAME` if you want to
select it explicitly, including when the server renames `ask_ai` to `bro`.

## Conversation

Type a prompt at the terminal and press Enter. Responses appear as they
arrive, with exposed reasoning shown separately. `--no-reasoning` hides
reasoning in the terminal; it does not change generation or server archives.
The client retains completed user prompts and final answers for the current
conversation and restores them on restart when history is enabled. Earlier
visible turns are sent as native provider chat
messages with each new prompt. Intermediate tool calls and reasoning are not
replayed across user turns; the normal tool loop runs within each request.
`--system TEXT` overrides the server's system instructions for this chat.

Gemini Interactions requires prior thought signatures and tool steps for
[stateless continuation](https://ai.google.dev/gemini-api/docs/text-generation#stateless-conversations).
With this history format, those survive only within one ask call. Enabling
provider storage with `--extra` does not supply continuation between CLI
prompts: the client does not retain or send a `previous_interaction_id`.

The model can call the same tools that AskAI lends it for an ordinary ask
request. Enable those tools in the server configuration, for example
`--upstream-tools --notes /path/to/session.git`. Notes and automatic
transcripts use the server's configured repositories and participant.

| Command | Effect |
| --- | --- |
| `/clear` | Clear the client's conversation history. |
| `/regen` | Replace the last successful answer after a new answer succeeds. |
| `/history` | Display the restored and current completed conversation. |
| `/read PATH` | Read a UTF-8 prompt from a file. |
| `/tools [NAME]` | List accessible tools, or show a tool's description and input schema. |
| `/tool NAME key=VALUE ...` | Call a tool directly; a JSON object is also accepted. |
| `/names` | List cached known session participants. |
| `/refresh` | Refresh accessible tools, participants and notebook filenames. |
| `/help` | Show commands. |
| `/exit`, `/quit` | Leave the client. |

The terminal editor uses GNU Readline and its normal Emacs key bindings by
default:

| Key | Effect |
| --- | --- |
| Up / Down | Browse input history. |
| PgUp / PgDn | Search input history by prefix. |
| Alt-Up / Alt-Down, Ctrl-PgUp / Ctrl-PgDn | Scroll the selected pane backward/forward. |
| Alt-Left / Alt-Right | Select conversation / reasoning for scrolling. |
| F6 | Switch the selected pane. |
| Ctrl-End | Follow new output in the selected pane and clear its unread count. |
| Ctrl-_ | Undo an edit, including clearing the draft with Ctrl-C. |
| Ctrl-R | Search history backward as you type; repeat to find older matches. |
| Home / End, Ctrl-A / Ctrl-E | Move to the beginning/end of the line. |
| Ctrl-left / Ctrl-right, Alt-B / Alt-F | Move by words. |
| Ctrl-U / Ctrl-K | Cut to the beginning/end of the line. |
| Ctrl-W / Alt-D | Cut the preceding/following word. |
| Ctrl-Y | Paste the last cut text. |
| Alt-. | Insert the last argument from the previous history entry. |
| Tab | Complete commands, paths, tools, parameters, values and participant names; press twice to list choices. |

Type `Explain` and press PgUp to recall
earlier prompts with that prefix, skipping unrelated entries. Up and Ctrl-R include saved input
from earlier client processes. Input recall is separate from the completed
conversation sent to the model; `/clear` clears the conversation, not
Readline history. A multiline prompt is recalled as one entry. Recall retains
up to 256 submitted entries, with a 1 MiB budget for their submitted text.
Readline can retain edits to recalled entries beyond that budget.

Readline honors `~/.inputrc` or the file named by `INPUTRC`, including
`set editing-mode vi`. These bindings override the defaults, including
PgUp/PgDn in full-screen mode. Use an application-specific block to customize
only this client. The `askai-scroll-up` and `askai-scroll-down` Readline
functions can be bound to alternate keys if the terminal intercepts a shortcut:

```text
$if askai-cli
set editing-mode vi
$endif
```

A trailing `\` continues the prompt on another line. With
`--multiline-input`, Enter continues the prompt and a trailing `\` submits
it instead. A trailing `/` submits in either mode. Ctrl-D ends input.

While an answer runs in full-screen mode, Enter queues prompts in submission
order and opens a fresh draft. The status row shows the queued count. The
queue holds up to 32 lines or 16 MiB; a full queue reports an error and keeps
the draft editable. Local commands remain available with prompts queued: `/help`,
`/history`, `/names`, and `/tools` display their current data immediately.
`/quit` and `/exit` initiate shutdown without waiting for generation.
`/clear` and `/regen` cancel the current turn before changing conversation
state. `/read` and `/refresh` wait for the current operation. `/tool` starts
independently of generation and other manual tool calls; results appear as
they arrive and stay out of conversation history. Up to 16 requests may be
outstanding, with a separate reserve for cancellation. Further tool calls
report the capacity limit and remain available in input history for retry.
Redirected input, `--simple-io`, and `--single-turn` retain sequential execution.

Completion choices show only the word or value being completed, without
repeating earlier words or a parameter's `name=` prefix. Choices appear
across each row, wrapping at the conversation pane's width, and remain in
scrollback across redraws. Commands, tools, parameters, values, participants
and files have separate palette styles. Every listing requires a fresh pair
of Tab presses within 400 ms; repeated single Tabs do not repeatedly print
the choices. A unique completed word gets one separating space, with the
cursor ready for the next word. The second Tab of that pair leaves it there.
Directory completion ends with `/` so the path can continue.

Ctrl-C cancels the foreground operation without discarding the draft being
edited or cancelling independent manual tools. When only manual tools are
active, it cancels the most recent one. At an idle prompt it clears the draft;
Ctrl-_ restores it. `/quit` and `/exit` cancel all outstanding requests.
In `--single-turn` mode or with redirected input, cancellation exits with
status 130.
SIGTERM exits with status 143. Failed or cancelled turns do not enter the
conversation history, and `/regen` preserves the old answer if regeneration
fails. Regeneration is a new request and may run tools again.

The full-screen display has a conversation pane on the left and a separate
reasoning pane on the right, with 256-color foreground styles on the terminal's
default background. Input stays on the bottom row from startup, including
while connecting. Each pane has its own scroll position and unread count.
Alt-Left selects conversation and Alt-Right selects reasoning. Alt-Up/Down
or Ctrl-PgUp/PgDn scroll the selected pane; Ctrl-End follows new output.
Input editing remains active. Both panes stay visible in narrow terminals,
with their text rewrapped to fit. Terminals smaller than 8 columns or 6 rows
show only input until resized; both buffers and the draft are retained.

Each scrollback retains up to 10,000 logical lines or 16 MiB of text.
Older transcript content is evicted independently of drafts and
saved input history. Scrolling away from the bottom preserves the viewport
and shows an unread count; Ctrl-End resumes following new output. Colors are
retained with text across scrolling and resizing. Adjacent text with the
same style shares one run. Rendering updates changed rows in changed regions;
editing the draft does not redraw scrollback. The cursor remains visible
during output updates.

The client imposes no generation timeout. AskAI's configured upstream timeout
and context limits still apply. A chat request stays on its event stream even
when the server has `--background` enabled; the server's native provider
background mode remains enabled. The server's configured notes and transcript repositories
retain their contents, including when `/clear` starts a new conversation.

History also counts toward the server's `--max-body` and `--max-values`
request limits. For longer conversations, increase those limits in the
server configuration (for example `--max-values 8192`), or use `/clear`.
The history API accepts at most 1024 earlier messages.

## Client history repository

Interactive clients save input and completed turns in a private bare Git
repository, separate from the server's notes and transcript repositories.
The default is `$XDG_STATE_HOME/askai/cli.git`, or
`~/.local/state/askai/cli.git` when `XDG_STATE_HOME` is unset. Git must be
installed. Use `--history-repo PATH` to select a dedicated repository and
`--no-history` to disable client persistence. Noninteractive runs do not
persist by default; an explicit `--history-repo` enables it for scripts.

Submitted commands and prompts are committed before execution. Completed
answers are saved separately after success; failed and cancelled input stays
in recall without becoming a completed model turn. Restoring input never
executes it. `/read` records the command as input and, on success, the file's
contents as the prompt. `/regen` keeps the previous successful answer until
its replacement succeeds. `/clear` saves an empty conversation while retaining
input recall and prior Git commits.

History is associated with the server's session and participant, the selected
ask tool, and the system-instruction override. Each terminal gets its own
writer branch under `refs/heads/cli/`; a `latest` ref selects the snapshot used
to restore a conversation. Concurrent terminals keep separate conversations.
Startup recall combines exact-text-deduplicated input from up to 32 recent
writer tips. The active snapshot holds at most 512 completed turns and 16 MiB
of prompt/answer text. Older entries remain reachable through Git history.

The server advertises a hash of its canonical shared notes repository path as
the session key, so instances sharing that path share a session. This is a
path identity, not a portable repository UUID: moving the repository changes
it, while identical paths on different hosts can collide. Older servers and
servers without notes fall back to the canonical socket path. Participant
names are also identifiers, not authentication; changing a name selects
different client history.

Use `--session-id ID` to supply a stable explicit session key when relocating
a repository or distinguishing otherwise identical paths on different hosts.
Use the same value in clients that should share history. A metadata refresh
that changes the server's session, participant or selected ask tool stops the
client and requests a restart, preventing old conversation context from being
sent into the newly identified session.

Each commit includes a bounded binary `snapshot` plus readable `identity.txt`,
`input.txt`, and, when available, `prompt.txt` and `answer.txt`. For example,
`git --git-dir=/path/to/cli.git log --all --stat` shows archived changes.
If committing fails after a complete snapshot has been synced, the client
reports the recovery filename and leaves it in the repository. It does not
silently execute input whose save failed. Recovery snapshots are retained
for manual recovery; startup selects committed state.

Git makes the archive inspectable and preserves concurrent branches, but it
does not provide remote backup or secure deletion. `/clear` does not purge
secrets from old commits. The repository grows beyond the bounded recall
window and needs an explicit retention policy if long-term storage must be
limited. Client snapshots contain submitted input and completed answers;
provider identifiers remain in the server's private recovery checkpoints,
and decoded reasoning in its reasoning archive. Raw response events are not
archived. A client restart restores conversation context,
not a live provider stream.

Commits run synchronously at human input and completed-answer boundaries.
This favors durable input before side effects; slow storage can delay those
boundaries. It is not a suitable per-token persistence strategy. The server's
stream journal has a different workload and remains separate.

## Tools and participant autocomplete

Tool completion uses the definitions returned by `tools/list`, including
the access restrictions of the current connection. For example:

```text
/tools
/tools notes
/tool notes action=list
/tool notes action=add file=plan.md text='Investigate the stalled request.'
/tool notes {"action":"read","file":"plan.md"}
```

After `/tool `, Tab completes tool names. After the name it completes
top-level parameter names as `name=`, omitting parameters already supplied.
After `=`, it completes schema enum and constant values, booleans, known
participant names for `participant`, `recipient` and `author`, and discovered
selectors for `notebook`. `/tools ` also completes tool names. Completion
preserves the other arguments when editing in the middle of a line.

For notes tools, `file=` completes names from the selected notebook, including
when `notebook=` follows the cursor. Read and attach default to `own`; copy
defaults to `other`. Add, list and notebooks actions do not offer existing
filenames. Local filesystem completion is limited to `/read PATH`, which
reads a local prompt file; it does not supply notes paths.

The `key=VALUE` form accepts single or double quotes and backslash escaping;
it performs no shell expansion or execution. String parameters remain
strings, while numbers, booleans, arrays and objects use JSON syntax. For
example, `enabled=true`, `limit=20` and `options='["--stat"]'` supply typed
values when those types appear in the schema. Use the full JSON-object form
for complex arguments. The shorthand checks required parameters and basic
types; full JSON objects go to the server for validation. For a string/null
union, unquoted `null` is JSON null and quoted `'null'` is a string. Direct
tool calls and their results do not enter the conversation sent to the model.

The participant list comes from the shared notes repository. The client
reads the notes tool's notebook selectors and deduplicates their participant
component; principal hashes and the `client`/`upstream` sides are not names.
The server also advertises its own configured participant before it has
written a note. These are known contributors, not an online-presence roster.
Without access to the notes tool there is no participant discovery. Older
AskAI servers can supply contributors through their existing `notebooks`
action, but cannot advertise themselves before their first note.

At the start of a prompt, typing `ali` and pressing Tab can produce
`alice: `; `@ali` completes to `@alice ` anywhere in the prompt. Chat name
completion uses only participants advertised as present by the connected
service. Currently that is its configured participant from notes metadata;
servers without that metadata supply no chat name candidates. Notebook
contributors remain available for tool argument completion and `/names`.
These completions do not route messages. Use `/refresh` to pick up another
instance's newly written notes.

Discovery runs at connection time and on `/refresh`. Notes filenames refresh
before the next prompt after an ask or a direct note mutation. Use `/refresh`
to see changes made by another client. A server's
`notifications/tools/list_changed` triggers a refresh before the next prompt.
Each connection owns its completion data; pressing Tab never makes an MCP
request. `/read` completion reads the local filesystem.

Autocomplete is implemented by the C modules `autocomplete.c` and
`autocomplete_file.c`. Their `.h` interfaces expose borrowed strings with
explicit byte lengths and C linkage. Each module retains its work buffers
and result storage until destruction, growing capacity as needed and keeping
it between calls. Text storage uses `dstr` and UTF-8 validation uses `jb_utf8`.
Candidate output is bounded to 256 entries and 16 MiB. The Readline adapter consumes
those views directly; only the strings that Readline takes ownership of are
copied. The terminal, display, and MCP transport are C modules with explicit
ownership and error returns; no C++ compiler or runtime is needed.

Tool calls and progress use IRC-style action lines, for example
`* you call notes` and `* ask_ai turn 1, calling git`. Reasoning remains in
the current transcript. Token-count progress is coalesced while text streams,
so it does not split reasoning into one block per delta. Actual tool actions
still appear at their stream boundary. Multiple connections, IRC-style windows and private
thought-stream tabs are future UI work.

## Initial prompts and scripts

Start a conversation with a prompt:

```sh
askai-cli --socket /path/to/askai.sock --prompt 'Explain this repository.'
```

Run one prompt and exit:

```sh
askai-cli --socket /path/to/askai.sock --single-turn \
  --no-reasoning --prompt 'Summarize the recent changes.'
```

Read a prompt from a file:

```sh
askai-cli --socket /path/to/askai.sock --single-turn --file question.txt
```

`-p` and `-f` are aliases for `--prompt` and `--file`. `--simple-io` disables
terminal input editing; this mode is also selected automatically when input
or output is not a terminal. `--color auto|on|off` controls terminal colors.
With `auto`, colors are enabled only on a terminal when `NO_COLOR` is unset.

## Checks and source attribution

`make check-cli` checks terminal input in a pseudo-terminal, argument
handling, and Unix-socket integration against a mock upstream, without
downloading a model. Environments that prohibit Unix sockets explicitly
skip the socket-dependent checks.

The C autocomplete tests also check counted input, C/C++ linkage, and
allocator activity after warming the reusable completion buffers.

The terminal console is derived from llama.cpp's MIT-licensed
`common/console.cpp` and `tools/cli/cli-ui.h` at commit
`b76b13379d3b7d67ab0bc424516bb6fd2522bd8e`;
the retained license is in [llama-cli-LICENSE](llama-cli-LICENSE). The
adaptation is in `src/cli_console.c`, `src/cli_console.h`, `src/cli_ui.c`, and
`src/cli_ui.h`.

## Event ownership

The main thread owns Readline, terminal state and scrollback. Its epoll loop
uses signalfd and one-shot timerfd deadlines. A separate network thread uses
the existing daemon loop unchanged, with libcurl socket/timer callbacks.
Blocking file and Git work uses the existing blocker pool; operations on one
history instance remain serialized. There is no periodic UI tick.

Mailboxes belong to explicit owner objects. Messages transfer ownership on
successful posting, with separate capacity for control traffic. Bounded
network credits pause streaming when the UI cannot consume output. No CLI
mailbox or owner lookup uses file-scope or thread-local state; Readline's
single callback bridge remains the exception for editor callbacks.

Shutdown keeps completion destinations alive until producers finish. If a
system call remains stuck past the termination deadline, the client restores
the terminal and exits without freeing storage still reachable by a worker.
Contended history locks retry on a blocker with a bounded cancellation delay;
they never hold up the UI loop.
