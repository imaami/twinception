# Terminal chat through AskAI

`askai-cli` adapts llama.cpp's terminal input interface into a standalone chat
client for AskAI. The configured upstream model answers directly, and AskAI
executes its permitted tools and stores its notes and transcripts. The client
contains no model loader, tokenizer, sampler, local inference engine, or
llama-server dependency.

## Build and install

The client needs a C++17 compiler, libcurl with Unix-socket support, json-c,
and their development packages. From the repository root:

```sh
make askai-cli
build/askai-cli --help
make install-askai-cli PREFIX="$HOME/.local"
```

`make askai-cli` builds only the client. `make` also builds AskAI and the
other programs. The targeted installer defaults to `/usr/local/bin`, accepts
`PREFIX`, `BINDIR`, and `DESTDIR`, and installs only `askai-cli`. To remove it:

```sh
make uninstall-askai-cli PREFIX="$HOME/.local"
```

The client links libcurl, json-c, and the C++ runtime. It does not link
libllama, ggml, or any inference backend.

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
The client retains completed user prompts and final answers in memory for the
current conversation. Earlier visible turns are sent as native provider chat
messages with each new prompt. Intermediate tool calls and reasoning are not
replayed across user turns; the normal tool loop runs within each request.
`--system TEXT` overrides the server's system instructions for this chat.

The model can call the same tools that AskAI lends it for an ordinary ask
request. Enable those tools in the server configuration, for example
`--upstream-tools --notes /path/to/session.git`. Notes and automatic
transcripts use the server's configured repositories and participant.

| Command | Effect |
| --- | --- |
| `/clear` | Clear the client's conversation history. |
| `/regen` | Replace the last successful answer after a new answer succeeds. |
| `/read PATH` | Read a UTF-8 prompt from a file. |
| `/help` | Show commands. |
| `/exit` | Leave the client. |

The terminal editor supports arrow-key history, Home/End, word navigation
with Ctrl-left/right, Backspace/Delete, and Tab completion for commands and
file paths. A trailing `\` continues the prompt on another line. With
`--multiline-input`, Enter continues the prompt and a trailing `\` submits
it instead. A trailing `/` submits in either mode. Ctrl-D ends input.

Ctrl-C cancels an answer and returns to the prompt. While editing input it
exits the client; in `--single-turn` mode cancellation exits with status 130.
SIGTERM exits with status 143. Failed or cancelled turns do not enter the
conversation history, and `/regen` preserves the old answer if regeneration
fails. Regeneration is a new request and may run tools again.

The client imposes no generation timeout. AskAI's configured upstream timeout
and context limits still apply. A chat request stays on its event stream even
when the server has `--background` enabled; the server's native provider
background mode remains enabled. Closing the client discards its in-memory
conversation. The server's configured notes and transcript repositories
retain their contents, including when `/clear` starts a new conversation.

History also counts toward the server's `--max-body` and `--max-values`
request limits. For longer conversations, increase those limits in the
server configuration (for example `--max-values 8192`), or use `/clear`.
The history API accepts at most 1024 earlier messages.

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
With `auto`, colors are enabled only on a terminal.

## Checks and source attribution

`make check-chat` checks argument handling and Unix-socket integration against
a mock upstream, without downloading a model. Environments that prohibit
Unix sockets explicitly skip the socket-dependent checks.

The terminal console is derived from llama.cpp's MIT-licensed
`common/console.cpp` and `tools/cli/cli-ui.h` at commit
`b76b13379d3b7d67ab0bc424516bb6fd2522bd8e`;
the retained license is in [llama-cli-LICENSE](llama-cli-LICENSE). The
adaptation is in `src/chat-console.cpp`, `src/chat-console.h`, and
`src/chat-ui.h`.
