# The configuration file of askai

`askai --config FILE` (`-c FILE`) reads options from FILE before it reads
the command line.  This document describes every member that the file can
have: the form of its value, the values that `askai` accepts, its default,
and what it does.  [`askai.example.json`](askai.example.json) is a complete
file that sets every option, and has a plugin, the member `tools` and a
policy (section [The example file](#the-example-file)).  Section
[Examples](#examples) has three short files.  [`README.md`](../README.md)
describes how `askai` works, and `askai --help` lists the options of the
command line.

## How askai reads the file

### Form

The file must be one JSON object in strict JSON, so that tools such as `jq`
can read it.  `askai` refuses a comment, a trailing comma, and characters
other than blanks after the object, and the error message names the line and
the column.  The parser does not refuse every form that strict JSON forbids:
it accepts a member name in single quotes, and the values `NaN` and
`Infinity`.

The object of the file can have these members:

- `upstream`, `service`, `testing` and `debugging` are objects that contain
  the options.  The name of a member of one of these objects is the long
  name of an option without the two dashes: `"max-turns": 8` in `upstream`
  is `--max-turns 8` on the command line.  Each option is a member of one
  of the four objects.  Section [Members](#members) lists the options of
  each object, and `askai --help` lists them in a section for each object.
- `tools` allows or denies tools to clients and to the upstream model
  (section [Tools](#tools)).
- `roles`, `identities`, `unix-groups` and `anonymous` define who may
  connect (section [Who may connect](#who-may-connect)).
- `plugins` lists the plugins that `askai` loads (section
  [Plugins](#plugins)).

This file sets the upstream, two options of `service` and one of
`debugging`:

```json
{
  "upstream": { "url": "http://127.0.0.1:8081/v1/chat/completions" },
  "service": { "listen": "127.0.0.1:8090", "man": true },
  "debugging": { "verbose": true }
}
```

If a member is in the wrong place, `askai` stops with exit status 2.  The
message begins with the path of the file and names the member:

| Message | Cause |
| ------- | ----- |
| `url must be in upstream` | an option at the top level, or in another of the four objects than its own |
| `upstream: unknown member bogus` | a member of one of the four objects that is not an option, also `config` and `help` |
| `unknown member bogus` | an unknown member at the top level, also `config` and `help` |
| `upstream must be an object` | one of the four objects is not an object |

If two members of one object have the same name, `askai` uses the last
one.

A value has one of these forms:

- The value of an option with an argument is a string or a number.  A number
  must be an integer, except for `temperature`: `"slots": 2.0` is refused.
- A size is an integer, or a string with a decimal integer that may end in
  `K`, `M` or `G` for KiB, MiB or GiB.  The unit must be uppercase, and a
  size with a unit must be a string: `"max-body": "4M"` is 4194304 bytes.
- The value of an option without an argument is `true`.  `false` changes
  nothing.  Any other value is refused, also the string `"true"`.
- The value of a repeatable option (`header`, `allow-origin`, `listen` and
  `git`) is one value or an array of values.
- The value of `extra` is an object, or a string that contains a JSON object.
- `null` is refused as the value of an option.

JSON in the file may nest at most 32 levels, the object of the file
included.  If a value nests deeper, `askai` stops with exit status 2 and the
message "nesting too deep".

### The file and the command line

`askai` applies the file first and the command line second.  The two
combine as follows.

- An option with one value on the command line replaces the value of the
  file.
- `--header`, `--allow-origin` and `--git` on the command line add to the
  lists of the file.
- Any `--listen` on the command line replaces the whole `listen` list of
  the file.
- `--extra` on the command line replaces the whole `extra` object of the
  file.  `askai` does not merge the two objects.
- The command line cannot turn off an option without an argument that the
  file sets to `true`.
- `askai` reads only the last `--config` of the command line.
- Only the file can set `tools`, `roles`, `identities`, `unix-groups`,
  `anonymous` and `plugins`.

`askai` checks the options against each other after it has applied both.
For example, `max-body` must not exceed `body-budget`, also when the file
sets one of them and the command line sets the other.

### Reload with SIGHUP

With `--config` or a `tls:` listener, SIGHUP makes `askai` read the command
line, the file and the TLS files again.  `askai` checks them as it does at
start, for example the git repositories, the notes directory, the API key
variable and the form of the listen addresses, with two exceptions: a
reload does not check `plugins`, and it does not open the listeners.  So a
file that a reload accepts can still stop `askai` at the next start.  If a
check fails, `askai` logs the error and changes nothing.  Otherwise `askai`
applies these members:

- `roles`, `identities`, `unix-groups` and `anonymous`.  The new policy
  applies to the requests that begin after the reload.  The requests under
  way finish under the old policy.
- `tls-cert`, `tls-key` and `tls-allow-rsa` of `service`, if `askai`
  serves a `tls:` listener.  New connections use the new certificate and
  key.

Every other member needs a restart, `plugins` and `tools` included.  If
another member has changed, the reload logs the warning "other changed
options need a restart".  After a reload, `tools` and `upstream` of a role
can name only the tools that the running `askai` offers.  `askai` reads the
variable of an `env:` token from its own environment, and a reload does not
change that environment.  So a new value of the variable, or a variable
that was not set when `askai` started, needs a restart.

Without `--config` and without a `tls:` listener, `askai` stops on SIGHUP
as it does on SIGTERM.  If `askai` starts with SIGHUP ignored, as `nohup`
sets it, SIGHUP stays ignored without `--config` and without a `tls:`
listener.  With either of them, `askai` reloads on SIGHUP in that case too.

### Permissions

`askai` refuses a configuration file that others than root and the user of
`askai` can change.  The rules are these:

- The file must be a regular file of at most 1 MiB.  The last component of
  its path must not be a symbolic link.  A directory in the path may be a
  symbolic link, and `askai` then checks the directories of the real path.
- The owner of the file must be root or the user of `askai`, and neither
  the group nor others may have write permission.
- The same rule applies to every directory above the file.  A directory
  that the group or others may write to is allowed if it has the sticky
  bit, as `/tmp` has.
- Others may read the file.

The files of `tls-cert` and `tls-key` must follow the same rules, and
others must not be able to read the file of `tls-key`.  The rules of owner
and write permission also apply to the directory of `notes`, or to the
directory above it if it does not exist yet, to the files of `plugins`, and
to every directory above each of them.  The path of a plugin must be the
real path of its file.

## What is on by default

### Which tools are on, without a file and with one

`askai` offers two sets of tools.

The first set is the tools of the clients.  A client lists them with
`tools/list` and calls them with `tools/call`, as its role allows.

| Tool | `askai` offers it |
| ---- | ----------------- |
| `ask_ai`, or the name that `tool-name` sets | always |
| `man` | with `"man": true` in `service` |
| `git` | with `git` in `service` |
| `notes` | with `notes` in `upstream` |
| `test_task` | with `"test-task": true` in `testing` |
| `background`, `jobs`, `wait` and `timer` | with `"client-jobs": true` in `service` |
| the tools of the plugins | with `plugins` |

The second set is the tools of the upstream model.  `askai` offers them
only with `"upstream-tools": true`, and only in the turns of an `ask_ai`
call (section [Tools for the upstream model, in
`upstream`](#tools-for-the-upstream-model-in-upstream)).
They are `man`, `git`, `notes` and the plugin tools whose kind has the flag
`TOOL_LENT` (`src/tool.h`), and `compact`, `background`, `jobs`, `wait` and
`timer`.  `compact` never appears in `tools/list`, and a role cannot name
it.  `background`, `jobs`, `wait` and `timer` appear in `tools/list` only
with `"client-jobs": true`, as tools of the clients.  A role can then name
them in `tools`, but never in `upstream`.  The turns of an `ask_ai` call
get these four tools with a definition of their own, and their jobs and
timers are not those of the client.

Without any of these options, `askai` offers only `ask_ai`.  A file does not
change that: a member of the file turns on a tool as the option of the same
name does, such as `"man": true` in `service` for `--man`.

The member `tools` turns no tool on.  It can deny each of these tools but
`ask_ai`, to clients, to the upstream model, or to both (section
[Tools](#tools)).

The policy sets which client may call which of the tools that `tools` allows
clients.  `askai` uses its own policy when there is no file, and when the
file has none of the members `roles`, `identities`, `unix-groups` and
`anonymous`:

- If the environment variable `ASKAI_TOKEN` is set and not empty, `askai`
  accepts only the clients that send `Authorization: Bearer TOKEN` with that
  value as TOKEN.  These clients may call every tool that `askai` offers,
  and they may read `GET /metrics`.
- Otherwise `askai` accepts every client, and every client may call every
  tool that `askai` offers.  Only clients on a Unix socket, and clients on
  loopback that send the `Host` `localhost`, `127.0.0.1` or `[::1]` with
  any port, or no `Host`, may read `GET /metrics`.  For each TCP listener
  that is not on loopback, `askai` logs a warning that clients without a
  token may call tools there.
- The upstream model may call every tool that `askai` can offer to the
  upstream and that `tools` allows it.

Any one of the four members turns `askai`'s own policy off, also with the
value `null`.  `askai` then ignores `ASKAI_TOKEN`, and logs a note about it.
Under a policy from the file, these rules apply:

- `askai` accepts a client only if the client sends the token of an
  identity, or connects as a Unix user that an identity or `unix-groups`
  names, or connects in a way for which `anonymous` names a role.  Other
  clients get the status 401.
- A client may list and call only the tools that its role lists in `tools`.
  A role without `tools` grants no tool.
- A client may read `GET /metrics` only if its role has `"metrics": true`.

So with `"man": true` and a policy in the file, `askai` offers `man` only
to the clients whose role lists `man` in `tools`.  With a policy in the
file, a client cannot list or call a tool that its role does not list, even
if `askai` offers the tool.  With `roles` alone in the file, `askai` refuses
every client.

### How to turn on compaction

Compaction has no member of its own.  `askai` offers the tool `compact` to
the upstream model in the turns of an `ask_ai` call when these conditions
are true:

1. `upstream-tools` is `true`.
2. The turns get at least one other tool that `tools` allows the upstream:
   `man`, `git` or a plugin tool that the role of the client allows the
   upstream to call, or `notes`, which the turns get in every call when
   `notes` is set.
3. The member `tools` allows `compact` to the upstream.  Without `tools`,
   it does.

So the smallest file with compaction is this one:

```json
{
  "upstream": {
    "url": "http://127.0.0.1:8081/v1/chat/completions",
    "upstream-tools": true,
    "notes": "/var/lib/askai/notes"
  }
}
```

`tools/list` does not list `compact`, because only the upstream model can
call it.  The member `tools` of each upstream request lists it.  In the
output of `GET /metrics`, the series
`askai_turn_tool_calls_total{tool="compact"}` counts the calls of
`compact`, and the series `askai_compactions_total` counts the compactions:
`by="upstream"` for a call of `compact`, and `by="askai"` for a full
context.  The upstream must accept tools in the request.  With
`llama-server`, its chat template must accept tools, as with `--jinja`.

The model calls `compact` with a summary of at most 4 MiB.  `askai` then
removes the conversation after the prompt, and adds the summary, the list of
the notes and the newest note to the prompt.  A newest note longer than a
tool result in a turn (section [`context`](#context)) is cut there, with the
offset from which the model reads on with `notes`.

`askai` also compacts the conversation without a summary when the context
is full.  The context is full in these cases:

- From the second turn on, the upstream returns an error whose text
  contains `context size`, `context length` or `context window`.  `askai`
  ignores case in this comparison.  On the first turn, `askai` ends the
  `ask_ai` call with such an error.
- The context size is known, the upstream reports its token usage, and
  after the results of a turn the counted tokens reach the context size.
- The request grows past 16 MiB.

`askai` compacts in this way only once until the upstream answers again.
If the context is full again before that, the `ask_ai` call ends with an
error.  `askai` counts the tokens from the usage that the upstream reports,
and at 4 bytes per token for the text that `askai` wrote after the last
answer.  If the upstream reports no usage, `askai` does not compact by the
count, and it computes the notices below at 4 bytes per token for the whole
request.

The context size is the value of `context`, or else `n_ctx` from the
`/props` of `llama-server`.  `askai` requests `/props` only if the upstream
can be `llama-server`: with `"provider": "llama"`, or with `url` and no
provider, and not with the APIs `generate` and `interactions`.  With
`slots`, `askai` requests `/props` only with `upstream-tools` and without
`context`.

With a known context size, `askai` adds a notice to the last result of the
tool calls of a turn:

| Context used | Notice |
| ------------ | ------ |
| a quarter | once, a reminder to write notes, if the turns have `notes` |
| a half | once, the same reminder |
| two thirds or more | after each turn, the fill level, and a request to write notes and then to call `compact` |

After a compaction, `askai` adds the reminders at a quarter and at a half
again.  If the context size is unknown, `askai` adds no notice, but the
model can still call `compact`.

### What the role member upstream means

The member `upstream` of a role lists the tools that the upstream model may
call in the turns of the `ask_ai` calls of the clients of the role.  It has
an effect only with `"upstream-tools": true`.

Without `upstream-tools`, the turns get no tool, and `upstream` must be
absent or `[]`.  With a tool name in `upstream`, `askai` then stops with
exit status 2 and a message such as `role user: askai offers no tool "man"
to the upstream`.

With `upstream-tools`, the member has these meanings:

- If a role has no `upstream`, the upstream may call the tools in the
  `tools` of the role that `askai` can offer to the upstream: `man`, `git`,
  `notes` and the plugin tools whose kind has the flag `TOOL_LENT`.  `askai`
  never offers `ask_ai` and `test_task` to the upstream.
- If `upstream` is `[]`, the upstream may call none of `man`, `git` and the
  plugin tools.  With `notes`, the turns still get `notes`, `compact`,
  `wait` and `timer`.  Without `notes`, the turns get no tool, and an
  `ask_ai` call is one upstream request, as without `upstream-tools`.
- If `upstream` lists tools, the upstream may call these tools.  Each name
  must be `man`, `git`, `notes` or a plugin tool whose kind has the flag
  `TOOL_LENT`, and `askai` must offer the tool in this run.  For example,
  `"upstream": ["man"]` needs `"man": true`.  The list may name tools that
  the `tools` of the role does not list.  The clients of the role then
  cannot call such a tool, but the upstream can call it for them.

In each case with `upstream-tools`, the turns also get these tools:

- `notes` in every call when `notes` is set, whatever the role lists;
- `compact`, `wait` and `timer` when the turns get any tool;
- `background` and `jobs` when the turns get a tool that can run in the
  background: `man`, `git`, or a plugin tool whose kind has the flag
  `TOOL_DETACH` (`src/tool.h`).

The member `tools` applies before these rules: a tool that `tools` denies
to the upstream does not count in them.  So the turns get `compact`,
`wait` and `timer` only beside another tool that `tools` allows, and
`background` and `jobs` only beside such a tool that can run in the
background.  The turns never get a tool that `tools` denies, whatever
`upstream` lists (section [Tools](#tools)).

Under `askai`'s own policy, the upstream may call every tool that `askai`
can offer to the upstream.

## Members

### The upstream AI, in `upstream`

These options are members of the object `upstream`.

#### `provider`

The value is a string: `llama`, `openai`, `anthropic`, `google`,
`deepinfra` or `openrouter`.  There is no default.  Without a provider,
`url` is required.

The provider sets the defaults of `api`, `url` and `key-env`.  The table in
section [Upstream providers and
APIs](../README.md#upstream-providers-and-apis) of `README.md` lists them.
`llama` is `llama-server` at `http://127.0.0.1:9931`.

#### `api`

The value is a string: `chat` (Chat Completions), `responses` (OpenAI
Responses), `messages` (Anthropic Messages), `generate` (Gemini
generateContent) or `interactions` (Gemini Interactions).  The default is
the API of the provider: `messages` for `anthropic`, `generate` for
`google`, and `chat` for the other providers and without a provider.
`google` also serves `chat` and `interactions`.

`api` sets the format of the upstream requests and answers.  `askai`
refuses an API that the provider does not serve, such as `"api":
"messages"` with `"provider": "openai"`.

#### `url`

The value is a string that begins with `http://` or `https://`.  The
default is the URL of the provider for the API.  Without a provider, there
is no default, and `url` is required.

`url` replaces the URL of the provider.  With `url`, `askai` does not read
the key variables of the provider (see `key-env`).  With the API
`generate`, if the path of the URL ends in `:generateContent`, `askai`
replaces that ending with `:streamGenerateContent`.  `askai` also sets the
query parameter `alt` to `sse`, and does not change the other query
parameters.

#### `model`

The value is a string.  There is no default.  Without `model`, `askai`
sends no model name.

With a provider other than `llama` and without `url`, `model` is required.
With the API `generate`, `askai` sends no model name in the request body.
With `"provider": "google"` and without `url`, `askai` adds the model to the
URL.  With `url` and the API `generate`, the URL must name the model, and
`model` changes only the default of `tool-description`.

#### `key-env`

The value is the name of an environment variable that contains the API
key.  `askai` computes the default.  At the URL of a provider, it is the
first variable of the provider that is set: `LLAMA_API_KEY` for `llama`,
`OPENAI_API_KEY` for `openai`, `ANTHROPIC_API_KEY` for `anthropic`,
`GEMINI_API_KEY` and then `GOOGLE_API_KEY` for `google`,
`DEEPINFRA_API_KEY` and then `DEEPINFRA_TOKEN` for `deepinfra`, and
`OPENROUTER_API_KEY` for `openrouter`.  With `url`, or without a provider,
it is `ASKAI_API_KEY`.

`askai` sends the key in the key header of the API: `Authorization: Bearer
KEY`, or `x-api-key: KEY` for `messages`, or `x-goog-api-key: KEY` for
`generate` and `interactions`.  If a `header` line has the same name, `askai`
sends that line instead of the key.

A variable that `key-env` names must be set and not empty, also when a
`header` line has the name of the key header.  Without `key-env`, `askai`
reads no key variable if a `header` line has the name of the key header,
such as `Authorization` for `chat`.  `askai` ignores case in this
comparison.  Otherwise a missing key is an error at the URL of a provider
other than `llama`.  With `url` or with `llama`, `askai` then sends no key.

#### `header`

The value is a header line, `"Name: value"`, or an array of header lines.
The default is no line.  The command line adds to the lines of the file.

`askai` sends the lines in each upstream request.  It also sends the header
of the API key, `anthropic-version: 2023-06-01` with the API `messages`,
and `User-Agent: askai/0.1`, each unless a line has that name.  The name
must not be empty or contain blanks, and the line must not contain CR or LF.

#### `system`

The value is a string.  The default is no system prompt.

`system` sets the default system prompt of an `ask_ai` call.  The argument
`system` of a call replaces it.  With an empty string, `askai` sends no
system prompt.

#### `temperature`

The value is a number from 0, or a string that contains such a number.  It
may have a fraction.  The default is none, and `askai` sends no
temperature.

`temperature` sets the sampling temperature of each upstream request.
`askai` refuses `NaN`, `Infinity` and a number that is too large for a
double, such as `1e400`.

#### `max-tokens`

The value is an integer from 1 to 2147483647.  The default is none, and
`askai` sends no limit, except with the API `messages`, which requires one.
With `messages`, the default is 4096.

`max-tokens` sets the limit of output tokens of each upstream request.
`askai` marks an answer that the limit cuts off.

#### `extra`

The value is a JSON object, or a string that contains one.  The default is
none.

`askai` merges the object into the body of each upstream request,
recursively.  For a member with the value `null`, `askai` removes that key
from the body.  The numbers of the object must be finite: `askai` refuses
`NaN`, `Infinity` and a number that is too large for a double.  `extra`
cannot turn off `"stream": true`.  With `upstream-tools`, `extra` must not
contain the conversation of the API: `messages` for `chat` and `messages`,
`input` for `responses` and `interactions`, and `contents` for
`generate`.

OpenAI reasoning models require the request member `max_completion_tokens`
instead of `max_tokens`.  With the API `chat`, `askai` sends `max_tokens`
only if `max-tokens` is set.  Without `max-tokens`,
`{"max_completion_tokens": 4096}` is enough.  With `max-tokens`,
`{"max_tokens": null, "max_completion_tokens": 4096}` removes `max_tokens`
and adds `max_completion_tokens`.

#### `timeout`

The value is an integer number of seconds from 0 to 2147483.  The default
is 600.  0 means no limit.

`timeout` limits each upstream call twice: first the wait for an upstream
slot, then the call itself.  Each turn of an `ask_ai` call is an upstream
call of its own.

#### `chunk-timeout`

The value is an integer number of seconds from 0 to 2147483.  The default
is 60.  0 means no limit.

`chunk-timeout` limits the longest pause in an answer after the answer has
begun.  After a longer pause, `askai` ends the call with an error.

#### `slots`

The value is an integer from 1 to 65535.  `askai` computes the default:
`total_slots` from the `/props` of `llama-server`, or else 4.

`slots` sets how many upstream calls `askai` makes at once.  The other
calls wait in a queue.  `askai` requests `/props` only with `"provider":
"llama"`, or with `url` and no provider, and not with the APIs `generate`
and `interactions`.

#### `max-calls`

The value is an integer from 1 to 1000000.  The default is 256.

`max-calls` limits the upstream calls that are queued or running.  At most
8 calls can be queued for one principal: one identity, one Unix user, or
else one MCP session or client address.  An `ask_ai` call past these limits
fails at once with an error, and the client can try again later.

### Tools for the upstream model, in `upstream`

These options are members of the object `upstream`.  With
`"upstream-tools": true`, `askai` runs each `ask_ai` call as a loop of
turns.  Each turn is an upstream call, and in its answer the model may call
tools.  `askai` runs the calls, writes their results into the conversation,
and sends the next request.  An answer that calls no tool ends the `ask_ai`
call, except in one case.  If the answer has text, the turn is not the
last, and the context is not full, `askai` waits while a job of
`background` runs or a timer is set.  When a job ends or a timer expires,
`askai` sends the next request, which reports the event to the model.  If
such an event has not been reported to the model yet, `askai` sends the next
request at once.  `askai` uses no upstream slot while it waits.

Besides `man`, `git` and the tools of plugins, the turns can get these
tools:

| Tool | What it does | When the turns get it |
| ---- | ------------ | --------------------- |
| `notes` | adds, lists and reads the notes of a thread | in every call with `notes` |
| `compact` | replaces the conversation after the prompt with a summary | with any other tool |
| `background` | runs a call of a tool as a job, and returns the number of the job at once | with a tool that can run in the background |
| `jobs` | lists the jobs, returns the output of a finished job, or cancels a job | with a tool that can run in the background |
| `wait` | waits up to 10 minutes for a job or a timer | with any other tool |
| `timer` | sets a timer of up to an hour, with a note | with any other tool |

Section [What the role member upstream
means](#what-the-role-member-upstream-means) describes which tools the turns
get.  The section [Lending tools to the
upstream](../README.md#lending-tools-to-the-upstream) of `README.md`
describes the turns in detail.

#### `upstream-tools`

The value is `true`.  The default is off.

With `upstream-tools`, `askai` offers tools to the upstream model in
`ask_ai` calls.  If the turns of a call get no tool, the call is one
upstream request.  The chat template of `llama-server` must accept tools,
as with `--jinja`.  `upstream-tools` also changes the default of
`tool-description`.

#### `notes`

The value is the path of a directory.  The default is none, and `askai`
offers no tool `notes`.

With `notes`, `askai` offers the tool `notes`, which stores notebooks in git
repositories under the directory.  If the directory does not exist, `askai`
makes it with mode 0700 when it starts.  The directory above it must exist,
or `askai` stops with exit status 2.  Older versions of `askai` used the
directory itself as one git repository for all notes.  If the directory
contains `HEAD`, as such a repository does, `askai` refuses it and stops
with exit status 1.  Clients get `notes` as their role allows.  With
`upstream-tools`, the turns of every `ask_ai` call get `notes`, and a call
can continue an earlier thread of notes with its argument `thread`.  A
thread has a notebook of the client and one of the upstream model under one
id.  Each side adds notes only to its own notebook, reads the other's with
`"notebook": "other"`, and copies a note of the other's into its own, byte
for byte, with the action `copy`.  With the APIs `generate` and
`interactions`, the model also gets the action `attach`, which uploads a
note to Gemini's Files API at the root of the API in the upstream's URL,
the part before `/v1beta/`, and gives it
to the model as a file.  A client adds a file that its program
attaches, of at most 4 MiB, with the argument `content`, by the MCP
extension `io.modelcontextprotocol/files` (section [Lending tools to the
upstream](../README.md#lending-tools-to-the-upstream) of `README.md`).  The
directory and the directories above it must pass the rules of section
[Permissions](#permissions).

#### `max-turns`

The value is an integer from 1 to 1000.  The default is 32.

`max-turns` limits the upstream calls of one `ask_ai` call with
`upstream-tools`.  Before the last turn, `askai` adds a notice that the next
turn is the last.  If the last answer still calls tools, `askai` ends the
call, with an error if the answer has no text.

#### `context`

The value is an integer number of tokens from 0 to 2147483647.  The default
is 0, and with 0 `askai` computes the size: `n_ctx` from the `/props` of
`llama-server`, or `inputTokenLimit` from `GET /v1beta/models/MODEL` of
Gemini's API with the APIs `generate` and `interactions` and a `model`, or
else unknown.

`context` sets the context size of the upstream model.  `askai` uses it
with `upstream-tools` in three ways:

- A tool result in a turn is a window of an eighth of the context, counted
  at 4 bytes per token, but at least 2 KiB and at most 64 KiB.  Without a
  known context size, the window is 64 KiB.
- `askai` adds the notices of section [How to turn on
  compaction](#how-to-turn-on-compaction).
- `askai` compacts the conversation when the counted tokens reach the
  context size.

### The ask tool, in `upstream`

These options are members of the object `upstream`.  They set the name and
the description of the tool with which clients ask the upstream model.

#### `tool-name`

The value is a string of 1 to 128 characters from `A-Z`, `a-z`, `0-9`, `_`,
`.` and `-`.  The default is `ask_ai`.

`tool-name` sets the name of the ask tool, and a role names the ask tool by
this name.  The name must differ from the names of the other tools that
`askai` offers in this run.  Several `askai` processes with different names
offer a model a choice, such as `ask_reasoner` and `ask_coder`.

#### `tool-description`

The value is a string.  `askai` computes the default from `model`,
`upstream-tools` and `notes`:

> Ask another AI model a question or give it a task, and get its answer. It
> cannot see this conversation, your files or your tools, so write a
> complete, self-contained prompt. Use it for a second opinion, for hard
> problems, or for knowledge you lack.

With `model` set to M, the text begins with "Ask the AI model M".  If the
turns of an `ask_ai` call can get a tool, `askai` adds "It may use some of
this server's tools itself, and its answer lists the calls it made." after
the second sentence.  If they can get `notes`, it then also adds "It keeps
notes in a thread, which a later call may go on with."  The turns can get
a tool with `upstream-tools`, when `askai` offers a tool that it can lend
and `tools` allows the upstream that tool.

### MCP server and listeners, in `service`

These options are members of the object `service`.

#### `listen`

The value is a listen address, or an array of listen addresses.  The
default is `["127.0.0.1:8090"]`.  Any `--listen` on the command line
replaces the whole list of the file.

An address has one of these forms:

- `[host:]port` is a TCP address.  Without a host, `askai` listens on
  `127.0.0.1`.  With an empty host, as in `:8090`, `askai` listens on the
  first wildcard address that getaddrinfo() returns and that works.  With
  glibc, that is `0.0.0.0`, for IPv4 only.  With `[::]:8090`, `askai`
  listens on the IPv6 wildcard address, and also accepts IPv4 connections
  unless `net.ipv6.bindv6only` is 1.  An IPv6 address is in brackets, as in
  `[::1]:8090`.  For a host name, `askai` listens on the first of its
  addresses that works.
- `unix:PATH` is a Unix socket.  PATH has 1 to 107 bytes.  The mode of the
  socket file follows the umask of `askai`.  `askai` replaces a socket file
  that no server listens on, and removes the file as it stops.
- `tls:[host:]port` is a TCP address with TLS 1.3 only, which needs
  `tls-cert` and `tls-key`.  A client that offers ALPN must offer
  `http/1.1`.  `askai` refuses `tls:unix:PATH` with exit status 2.

The port is an integer from 0 to 65535.  `askai` checks the form of each
address at start, and stops with exit status 2 at an invalid address.
If `askai` cannot resolve a host name or listen on an address, it stops
with exit status 1.

#### `tls-cert`

The value is the path of a file.  The default is none.

The file contains the certificate chain for the `tls:` listeners, in PEM:
the certificate of the server first, then the intermediate CA certificates.
`askai` sends the chain to clients as it is.  `askai` reads the file only
with a `tls:` listener, and reads it again on a reload.  The file must pass
the rules of section [Permissions](#permissions).

#### `tls-key`

The value is the path of a file.  The default is none.  A `tls:` listener
requires it.

The file contains the private key of the certificate, in PEM and not
encrypted: ECDSA on P-256, P-384 or P-521, or Ed25519.  An RSA key needs
`tls-allow-rsa`.  The key must match the certificate in `tls-cert`, or
`askai` stops with exit status 2.  On a reload, `askai` logs this error and
changes nothing.  The file must pass the rules of section
[Permissions](#permissions), and others must not be able to read it.  It
may be the same file as that of `tls-cert`.

#### `tls-allow-rsa`

The value is `true`.  The default is off.

`tls-allow-rsa` allows an RSA or RSA-PSS key in `tls-key`.  An RSA
handshake uses about three times the CPU time of an ECDSA handshake.

#### `allow-origin`

The value is an Origin, or an array of Origins.  The default is none.  The
command line adds to the Origins of the file.

`askai` allows a browser request whose `Origin` field is exactly one of
these values, such as `https://example.com` without a slash at the end.
With `"*"`, `askai` allows any Origin.  `askai` always allows loopback
Origins: `http` or `https` with the host `localhost`, `127.0.0.1` or
`[::1]` and any port.  A request with another `Origin` gets the status 403.

#### `loops`

The value is an integer from 0 to 255.  The default is 0, and with 0
`askai` computes the number: one fewer than the CPUs that `askai` may use,
as its CPU affinity and the CPU quota of its cgroup allow, at least 1 and
at most 255.

`loops` sets the number of event loops that serve clients.

#### `grace`

The value is an integer number of seconds from 1 to 3600.  The default is
5.

`grace` limits the time to finish the requests under way after SIGINT,
SIGTERM, or SIGHUP without a reload.  At the end of this time, `askai`
exits at once with status 3.

#### `memory-high`

The value is a size from 1 byte.  `askai` computes the default: a sixteenth
of RAM, or of the lowest memory limit of its cgroups, but at least 256 MiB.

From this resident memory on, memory pressure is high, and from twice this
value on, it is critical.  Under memory pressure, `askai` frees memory that
it has stored for reuse.

### Tools for clients, in `service`

These options are members of the object `service`.

#### `man`

The value is `true`.  The default is off.

With `man`, `askai` offers the tool `man`.  The tool returns a page of the
manual as `man(1)` renders it, in plain UTF-8 text.  `askai` renders each
page once and stores it.  With `upstream-tools`, the upstream may call
`man` too, as the role of the client allows.

#### `git`

The value is the path of a repository, or an array of paths.  The default
is no repository.  The command line adds to the paths of the file.

With `git`, `askai` offers the tool `git`, which runs read-only git
commands in these repositories.  Each path is the top of a work tree or a
bare repository.  The tool names each repository by the last component of
its path, without `.git`: `/srv/git/proj.git` is `proj`.  A name has 1 to
64 characters from `A-Z`, `a-z`, `0-9`, `_`, `.` and `-`, and does not
begin with a dot.  `askai` refuses two repositories with the same name.
With `upstream-tools`, the upstream may call `git` too, as the role of the
client allows.

#### `git-timeout`

The value is an integer number of seconds from 1 to 3600.  The default is
30.

At this deadline, `askai` sends SIGTERM to the process group of `git`, and
SIGKILL one second later, and the call fails.

#### `client-jobs`

The value is `true`.  The default is off.

With `client-jobs`, `askai` offers clients the tools `background`, `jobs`,
`wait` and `timer`, as their role allows.  `background` starts a call of
another tool of the client as a job, and answers at once with the job's id.
`jobs` lists the client's jobs and timers, returns the result of an ended
job, and cancels a job or a timer.  `wait` waits up to 10 minutes for a job
to end or a timer to fire, and `timer` sets a timer of up to an hour.  The
jobs and the timers belong to the client's identity, to the Unix user of a
client of a Unix socket without an identity, or else to the client's
address.  `askai` stores the result of a job for up to an hour after the job
ends.  An owner runs at most 8 jobs and tasks at once, and has at most 8
timers and 4 waits.  A change of `client-jobs` needs a restart.  The
section [Background jobs and timers for
clients](../README.md#background-jobs-and-timers-for-clients) of `README.md`
describes the tools in detail.

### Limits on clients, in `service`

These options are members of the object `service`.  They limit what each
client may send and what `askai` stores for it.  A client that exceeds a
limit gets an HTTP status, or `askai` closes the connection.  A client
address is an IPv4 address, an IPv6 /64, or the user of a Unix socket
client.

#### `max-head`

The value is a size from 1 to 65535 bytes.  The default is `"16K"`.

`max-head` limits the request line and the headers, and the trailer section
of a chunked body.  A longer head or trailer section gets the status 431.

#### `max-fields`

The value is an integer from 0 to 4294967295.  The default is 64.

`max-fields` limits the number of header fields, and the number of trailer
fields of a chunked body.  More fields get the status 431.  With 0, `askai`
refuses every request that has a header field.

#### `head-timeout`

The value is an integer number of seconds from 0 to 4294967295.  The
default is 10.  0 means no limit.

`head-timeout` limits the time of a TLS handshake, and the time of a
request line and headers, each from its first byte.  A late head gets the
status 408.

#### `max-body`

The value is a size.  The default is `"8M"`.  It must not exceed
`body-budget`, nor `client-budget` if that is set.

`max-body` limits a request body.  A larger body gets the status 413.  With
0, `askai` refuses every body.

#### `min-body-rate`

The value is a size per second, from 0 to 4294967295 bytes.  The default is
`"4K"`.  0 means no limit.

A request body must arrive at this average rate from the end of its
headers, once `body-grace` has passed.  A slower body gets the status 408.

#### `body-grace`

The value is an integer number of seconds from 0 to 4294967295.  The
default is 5.

`body-grace` sets the time after the headers before `min-body-rate`
applies.

#### `body-budget`

The value is a size.  The default is `"256M"`.  It must not be less than
`max-body`.

`body-budget` limits the request bodies in memory for all clients together.
A body past the budget gets the status 503 with `Retry-After: 1`.

#### `client-budget`

The value is a size from 1 byte.  `askai` computes the default: a quarter
of `body-budget`, but at least `max-body`.  It must not be less than
`max-body`.

`client-budget` limits the part of `body-budget` for one client address.  A
body past it gets the status 503.

#### `max-depth`

The value is an integer from 1 to 2147483646.  The default is 32.

`max-depth` limits the nesting of JSON in a request.  A deeper request gets
the status 400.

#### `max-values`

The value is an integer from 1 to 4294967295.  The default is 1024.

`max-values` limits the JSON values in a request, keys included.  json-c
uses up to 0.7 KiB for each value while it parses.  A request with more
values gets the status 400.

#### `max-members`

The value is an integer from 1 to 4294967294.  The default is 1024.

`max-members` limits the members of one JSON object in a request.  A larger
object gets the status 400.

#### `sse-keepalive`

The value is an integer number of seconds from 0 to 2147483647.  The
default is 15.  0 means no comment lines.

`sse-keepalive` sets the time between the comment lines that `askai`
writes on an idle event stream.

#### `max-client-conns`

The value is an integer from 0 to 4294967295.  The default is 256.  0 means
no limit.

`max-client-conns` limits the connections of one client address.  Over the
limit, a plain HTTP connection gets the status 503, and `askai` closes a
TLS connection without an answer.

#### `idle-timeout`

The value is an integer number of seconds from 0 to 4294967295.  The
default is 30.  0 means no limit.

`idle-timeout` limits the time that a connection may stay open with no
request under way, before its first request and between requests.  After
that time, `askai` closes the connection.

#### `output-pause`

The value is a size from 1 byte.  The default is `"256K"`.  It must not
exceed `max-output`.

When this much output is pending on a connection, `askai` reads no more
input from it, and pauses a response until at most half of this is
pending.

#### `max-output`

The value is a size from 1 byte.  The default is `"8M"`.

`askai` closes a connection if a response would make its pending output
larger than this size.  Only a response that is all of the pending output
may pass this size, and only while `askai` has not paused it
(`output-pause`).

#### `send-timeout`

The value is an integer number of seconds from 0 to 4294967295.  The
default is 30.  0 means no limit.

`send-timeout` limits the longest stall of pending output.  If the output
stalls longer, `askai` closes the connection.

### Test aids, in `testing`

These options are members of the object `testing`.  They exist for the
tests of `askai`.

#### `test-man-recheck`

The value is an integer number of seconds from 0 to 86400.  The default is
600.

When the lookup of the file of a man page (`man -w`) is older than this,
`askai` repeats the lookup in the background at the next `man` call for the
page.  With 0, `askai` repeats it at each call.

#### `test-stall`

The value is an integer number of milliseconds from 1 to 3600000.  The
default is none.  Without `test-stall`, `askai` does not handle SIGUSR2, so
the default action of the signal applies, and SIGUSR2 terminates `askai`.

With `test-stall`, on SIGUSR2 `askai` blocks each event loop in one turn
for this long.

#### `test-pressure`

The value is the path of a file.  The default is none.

`askai` reads the file each second.  Memory pressure is at least the level
in the file: 0 (none), 1 (high) or 2 (critical).

#### `test-fifo`

The value is `true`.  The default is off.

With `test-fifo`, upstream calls wait in one queue for each class, first in
first out, and without the limit of 8 queued calls for one principal.

#### `test-task`

The value is `true`.  The default is off.

`test-task` is a test aid.  With it, `askai` offers the tool `test_task`,
which runs CPU work in slices of 250 µs as a task on the client loops, or,
with a prompt, an upstream call as background work.

### Debugging, in `debugging`

These options are members of the object `debugging`.

#### `verbose`

The value is `true`.  The default is off.

Without `verbose`, `askai` logs as it starts the upstream, each tool and
whether it is on, each plugin with its tools, the tools of the upstream and
the policy, and it logs each tool call of a client that it refuses or that
fails.  Section [Log](../README.md#log) of `README.md` has an example.

`verbose` makes `askai` also log each tool call of a client as it ends,
with its arguments cut at 200 bytes, and each ask call as it starts and
ends.  It also logs each upstream turn, each tool call of a turn or of a
background job, each compaction, each notice about the context, each web
request of a plugin, and each man page that `askai` renders.

#### `watchdog`

The value is an integer number of milliseconds from 0 to 60000.  The
default is 20.  0 means no report.

`askai` logs a turn of an event loop that is longer than this.

### Tools

The member `tools` is at the top level of the file, beside the four objects
of the options.

#### `tools`

The value is `true`, `false`, a list of tool names, or an object.  The
default is `true`.

`tools` allows or denies the tools that `askai` offers in this run, to
clients and to the upstream model.  It turns no tool on: the options and
`plugins` do that (section [Which tools are on, without a file and with
one](#which-tools-are-on-without-a-file-and-with-one)).  The ask tool is
always on, and `tools` cannot name it.  The policy sets which clients may
call the ask tool.

The value has one of these forms:

| Value | The tools that it allows |
| ----- | ------------------------ |
| `true` | all |
| `false` | none |
| `["man", "git"]` | the tools of the list |
| `{ "allow": ["man", "git"] }` | the tools of the list |
| `{ "deny": ["man", "git"] }` | all but the tools of the list |
| `{}` | all |

Each of these forms applies to clients and to the upstream model.  An
object with the members `client` and `upstream` sets the two apart.  Each
of the two members has one of the forms of the table, and a missing member
allows all.  With this file, clients get every tool, and the upstream model
gets every tool but `godbolt_compile`:

```json
{
  "upstream": { "url": "http://127.0.0.1:8081/v1/chat/completions" },
  "tools": {
    "client": true,
    "upstream": { "deny": ["godbolt_compile"] }
  }
}
```

A name in a list must be a tool that `askai` offers in this run, other
than the ask tool: `man`, `git`, `notes`, `test_task`, `background`,
`jobs`, `wait`, `timer`, `compact` or a tool of a plugin, each only when
`askai` offers it.  `compact` exists with `upstream-tools`, and
`background`, `jobs`, `wait` and `timer` exist with `client-jobs` or
`upstream-tools`.  Otherwise `askai` stops with exit status 2.  The message
begins with the path of the file and `tools:`, or `tools: client:` or
`tools: upstream:` for those members:

| Message | Cause |
| ------- | ----- |
| `tools: askai has no tool "ls"; the tools are man, git` | a name of no tool, or of a tool that is off |
| `tools: ask_ai is the ask tool, which is always on` | the name of the ask tool |
| `tools: not a list of tools` | `allow` or `deny` is not an array |
| `tools: not true, false, a list or an object` | a value of another type |
| `tools: unknown member x` | a member other than `allow`, `deny`, `client` and `upstream` |
| `tools: allow and deny together` | an object with both |
| `tools: allow and deny go into client and upstream` | `allow` or `deny` beside `client` or `upstream` |
| `tools: background needs jobs` | a form that allows `background` but not `jobs`, with which the output of a job is read |

For clients, `tools` has these effects:

- A client cannot list or call a tool that `tools` denies to clients,
  whatever its role grants.  A role may name such a tool in `tools` or in
  `upstream` without an error.  Under `askai`'s own policy, every client
  gets the tools that `tools` allows clients.
- What `tools` allows clients does not change the tools of the upstream.
  A role without `upstream` still lends the upstream the tools that its
  `tools` lists, also those that `tools` denies to clients.
- The warning "no role lists these tools" at start leaves out the tools
  that `tools` denies to clients.

For the upstream model, `tools` has these effects:

- The turns of an `ask_ai` call get only the tools that `tools` allows the
  upstream.  This applies to every tool that the turns can get, also to
  `notes`, `compact`, `background`, `jobs`, `wait` and `timer`.  So
  with `"upstream": ["man"]`, the turns get `man` and no other tool: no
  compaction, no jobs and no timers.
- A denied tool does not count in the rules of section [What the role
  member upstream means](#what-the-role-member-upstream-means).  With
  `{"deny": ["man"]}` and no other tool to lend, the turns get no
  `compact`, `wait` or `timer`, and an `ask_ai` call is one upstream
  request.
- Without `notes` in the turns, an `ask_ai` call refuses the argument
  `thread`, and the ask tool does not list it.  From two thirds of the
  context on, the notice to call `compact` comes only when the turns have
  `compact`.
- Without `upstream-tools`, the turns get no tool, whatever `tools`
  allows.

As it starts, `askai` logs a tool that `tools` denies to clients as `off
(tools)`, in the list `built-in tools for clients:` and after the name of
a tool of a plugin.  The lists `upstream tools` of the policy show the
tools that the turns get.  A change of `tools` needs a restart.  A reload
checks the names of `tools` against the tools of the running `askai`.

### Who may connect

Any of these four members replaces `askai`'s own policy with the policy of
the file (section [Which tools are on, without a file and with
one](#which-tools-are-on-without-a-file-and-with-one)).  The names of roles
and identities have 1 to 64 characters from `A-Z`, `a-z`, `0-9`, `_`, `.`,
`@` and `-`.

#### `roles`

The value is an object.  Each member is a role: its name, and an object
with the members `tools`, `upstream` and `metrics`.  All three are
optional.  There is no default.

- `tools` is an array of the tools that the clients of the role may list
  and call.  The default is `[]`, no tool.  Each name must be a tool that
  `askai` offers in this run: the ask tool, `man`, `git`, `notes`,
  `test_task`, `background`, `jobs`, `wait`, `timer` or a tool of a
  plugin.  With any other name, `askai` stops with exit status 2, for
  example with `man` without `"man": true`, or with `compact`.  A role may
  name a tool that the member `tools` denies to clients, but its clients do
  not get that tool.
- `upstream` is an array of the tools that the upstream model may call in
  the turns of the `ask_ai` calls of these clients.  Section [What the role
  member upstream means](#what-the-role-member-upstream-means) describes
  it.
- `metrics` is `true` or `false`.  The default is `false`.  With `true`,
  the clients of the role may read `GET /metrics`.

#### `identities`

The value is an object.  Each member is an identity: its name, and an
object with the members `role`, `tokens` and `unix-users`.  There is no
default.

- `role` is the name of a role.  It is required.
- `tokens` is an array of tokens.  `"sha256:HEX"` is the SHA-256 digest of
  the token in 64 hexadecimal digits, as `printf %s TOKEN | sha256sum`
  prints it, so the file does not contain the token.  `"env:NAME"` names an
  environment variable of `askai` that contains the token.  If the variable
  is not set or is empty, `askai` stops with exit status 2.
- `unix-users` is an array of Unix users: user names, or uids as strings of
  digits or as integers.  `askai` looks up each user name when it reads the
  file.  If no user has the name, `askai` stops with exit status 2 at
  start.  A client on a Unix socket that sends no token is the identity
  that lists its user.

At least one of `tokens` and `unix-users` must not be empty.  A client
authenticates as an identity with `Authorization: Bearer TOKEN`.  If the
policy has any token, the token that a client sends determines its
identity, and an unknown token gets the status 401, also on a Unix socket.
If the policy has no token, `askai` ignores the tokens of the clients.  If
the identities list the same token twice, or the same Unix user twice, also
within one identity, `askai` stops with exit status 2.  A user name and its
uid are the same user.  All calls of an identity share one queue of the
scheduler.

#### `unix-groups`

The value is an object.  Each member maps a Unix group, by its name or by
its gid as a string of digits, to the name of a role.  `null` is refused.
`askai` looks up each group name when it reads the file.  If no group has
the name, `askai` stops with exit status 2 at start.  There is no default.

A client on a Unix socket that sends no token and that no identity names
gets the role of a group that the client is a member of.  If the client is
a member of several groups of `unix-groups`, `askai` uses the role of the
first of them in the file.  The groups of the client are its primary group
and its supplementary groups at the time it connected.

#### `anonymous`

The value is an object with the members `loopback`, `remote` and `unix`.
Each value is the name of a role or `null`.  If a member is missing or
`null`, `askai` refuses those clients.  There is no default.

`anonymous` sets the role of a client that sends no token, and of a client
that sends a token when no identity has `tokens`.  The member `loopback`
applies to a client over TCP from a loopback address that sends the `Host`
`localhost`, `127.0.0.1` or `[::1]` with any port, or no `Host`.  The member
`remote` applies to any other client over TCP.  The member `unix` applies
to a user of a Unix socket that no identity and no group of `unix-groups`
names.

If `remote` names a role with tools, `askai` logs a warning for each TCP
listener that is not on loopback, because clients without a token may call
tools there.

### Plugins

#### `plugins`

The value is an array.  The default is no plugin.  Each element is the
absolute path of a plugin, or an object with the member `path`, that path,
and the optional member `options`, an object of the options of the plugin.
With this file, `askai` loads two plugins and passes options to the second
one.

```json
{
  "upstream": { "url": "http://127.0.0.1:8081/v1/chat/completions" },
  "plugins": [
    "/usr/libexec/askai/one.so",
    { "path": "/usr/libexec/askai/godbolt.so",
      "options": { "url": "https://godbolt.org", "share": "short" } }
  ]
}
```

`askai` loads the plugins as it starts, in the order of the array.  On a
reload, `askai` loads no plugin.  The path must be the real path of the
file, without a symbolic link, and the file must pass the rules of section
[Permissions](#permissions).  `askai` offers at most 128 tools in all, and
the names of the tools must differ.

`askai` refuses an element that is not an absolute path or an object, an
object without `path` or with another member than `path` and `options`,
`options` that is not an object, `options` for a plugin that has no
options, and options that the plugin refuses.  If `askai` refuses a plugin,
it stops with exit status 2.  The section [Plugins](../README.md#plugins)
of `README.md` describes plugins and the options of the godbolt plugin.

## The example file

[`askai.example.json`](askai.example.json) has every member that the file
can have.  It sets every option of the four objects, and it has a plugin,
the member `tools` and a policy.  So it shows the form of each member in
one file.  The numbers and sizes are the defaults, except `temperature`,
`max-tokens`, `slots`, `context`, `memory-high` and `test-stall`.  The file
turns on `upstream-tools`, `man`, `git`, `notes` and `client-jobs`.

The file does not work as it is.  Change or remove these members before
use:

| Member | Value in the file | What to do |
| ------ | ----------------- | ---------- |
| `provider`, `url` and `model` in `upstream` | `llama`, a `llama-server` at `127.0.0.1:8081`, `local` | set the upstream to ask |
| `key-env` in `upstream` | `"LLAMA_API_KEY"` | name a variable that is set and not empty, or remove the member; otherwise `askai` stops with exit status 2 |
| `notes` in `upstream` | `"/var/lib/askai/notes"` | set a directory whose parent exists |
| `listen` in `service` | `127.0.0.1:8090`, `tls:8443` and `unix:/run/askai/askai.sock` | set the addresses; the directory of a Unix socket must exist |
| `tls-cert` and `tls-key` in `service` | files in `/etc/askai` | set your files, which the listener `tls:8443` needs, or remove them with that listener |
| `allow-origin` in `service` | `"https://example.com"` | set the Origins of your browser clients, or remove the member |
| `git` in `service` | `"/srv/git/proj"` | set repositories that exist |
| `test-stall` and `test-pressure` in `testing` | `100` and `"/run/askai/pressure"` | remove them: they are test aids |
| `path` of the plugin in `plugins` | `"/usr/libexec/askai/godbolt.so"` | set the real path of `godbolt.so`, or remove the plugin and the names of its tools in `tools` and `roles` |
| `unix-users` of the identity `operator` | `["root"]` | set the Unix users of the clients of the role `admin` |
| `tokens` of the identity `ci` | `"sha256:"` and 64 zeros | set the SHA-256 digest of a long random token; no token has the digest in the file |
| `unix-groups` | `{ "users": "user" }` | set your groups, or remove the member |

`test-fifo` and `test-task` are `false`, and `verbose` is `false`, so they
change nothing.  `tls-allow-rsa` is `false` too.

The file has these effects on the tools and the policy:

- `tools` allows clients every tool, and denies `godbolt_compile` to the
  upstream model.
- The role `admin` grants every tool and `GET /metrics`.  It has no
  `upstream`, so the upstream may call each tool of the role that `askai`
  can lend, but not `godbolt_compile`, which the member `tools` denies to
  the upstream.
- The role `user` grants `ask_ai`, `man` and `notes`.  Its `upstream`
  allows the upstream to call `man`, `git`, `notes`, `godbolt_catalog` and
  `godbolt_targets`.
- The identity `operator` is the Unix user `root` on the Unix socket, with
  the role `admin`.  The identity `ci` has the role `user`.  The members of
  the Unix group `users` get the role `user`.
- A client on loopback without a token gets the role `user`.  `askai`
  refuses a client without a token over TCP from another address, and
  another user of the Unix socket.

## Examples

### A minimal file

```json
{
  "upstream": {
    "url": "http://127.0.0.1:8081/v1/chat/completions"
  }
}
```

`url` is the only option that this file needs, because it has no provider.
`askai` listens on `127.0.0.1:8090` and offers the tool `ask_ai`, which
sends its prompt to the `llama-server` at `127.0.0.1:8081`.  Every client
may call `ask_ai`, under `askai`'s own policy.

### A file that offers tools to the upstream model

```json
{
  "upstream": {
    "url": "http://127.0.0.1:8081/v1/chat/completions",
    "upstream-tools": true,
    "notes": "/var/lib/askai/notes",
    "context": 131072
  },
  "service": {
    "man": true,
    "git": ["/srv/git/proj.git"]
  }
}
```

Every client may call `ask_ai`, `man`, `git` and `notes`, under `askai`'s
own policy.  The turns of each `ask_ai` call get `man`, `git`, `notes`,
`compact`, `background`, `jobs`, `wait` and `timer`.  `askai` stores the
notes that the model adds with the tool `notes` in git repositories under
`/var/lib/askai/notes`.  With a context of 131072 tokens, `askai` adds a
reminder to write notes at 32768 and at 65536 tokens, and a request to call
`compact` from 87382 tokens on.  A tool result in a turn is a window of
64 KiB.  The chat template of `llama-server` must accept tools, as with
`--jinja`.

### A policy

```json
{
  "upstream": {
    "url": "http://127.0.0.1:8081/v1/chat/completions",
    "upstream-tools": true
  },
  "service": {
    "listen": ["127.0.0.1:8090", "unix:/run/askai/askai.sock"],
    "man": true
  },
  "roles": {
    "admin": { "tools": ["ask_ai", "man"], "metrics": true },
    "user": { "tools": ["ask_ai"], "upstream": ["man"] },
    "guest": { "tools": ["ask_ai"], "upstream": [] }
  },
  "identities": {
    "alice": { "role": "admin", "tokens": ["sha256:9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08"] },
    "ci": { "role": "user", "tokens": ["env:ASKAI_CI_TOKEN"] },
    "bob": { "role": "user", "unix-users": ["bob"] }
  },
  "anonymous": { "loopback": "guest", "remote": null, "unix": null }
}
```

The objects `upstream` and `service` set the upstream URL, turn on
`upstream-tools` and `man`, and set two listen addresses: `127.0.0.1:8090`
and a Unix socket.  The members of the policy have these effects.

- The member `roles` turns `askai`'s own policy off.  A client may then call
  only the tools of its role.
- The role `admin` allows its clients to call `ask_ai` and `man`, and to
  read `GET /metrics`.  The role has no `upstream`, so in the turns of their
  `ask_ai` calls the upstream may call `man`, the one tool in `tools` that
  `askai` can offer to the upstream.  With `man`, the turns also get
  `compact`, `background`, `jobs`, `wait` and `timer`.
- The role `user` allows its clients to call only `ask_ai`.  The upstream
  may call `man` for them.
- The role `guest` allows its clients to call only `ask_ai`.  The upstream
  may call no tool, so each `ask_ai` call of a guest is one upstream
  request.
- The identity `alice` has the role `admin`.  A client that sends
  `Authorization: Bearer test` is `alice`, because `alice` lists the SHA-256
  digest of the token `test`.  Use a long random token instead.
- The identity `ci` has the role `user`.  A client that sends the token in
  the environment variable `ASKAI_CI_TOKEN` of `askai` is `ci`.  `askai`
  does not start if the variable is not set.
- The identity `bob` has the role `user`.  A client of the Unix user `bob`
  on the Unix socket is `bob` if it sends no token.  The user must exist.
- The member `anonymous` sets the role `guest` for a client on loopback
  without a token.  `askai` refuses a client without a token over TCP from
  another address, and another user of the Unix socket.
