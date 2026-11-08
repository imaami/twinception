# The configuration file of askai

`askai --config FILE` (`-c FILE`) reads options from FILE before it reads
the command line. This document describes every member that the file can
have: the form of its value, the values that `askai` accepts, its default,
and what it does. [`askai.example.json`](askai.example.json) is a complete
file that sets every option, and has a plugin, the member `tools` and a
policy (section [The example file](#the-example-file)). Section
[Examples](#examples) has three short files. [`README.md`](../README.md)
describes how `askai` works, and `askai --help` lists the options of the
command line.

## How askai reads the file

### Form

The file must be one JSON object in strict JSON, so that tools such as `jq`
can read it. `askai` refuses a comment, a trailing comma, and characters
other than blanks after the object, and the error message names the line and
the column. The parser does not refuse every form that strict JSON forbids:
it accepts a member name in single quotes, and the values `NaN` and
`Infinity`.

The object of the file can have these members:

- `agent` and `transcripts` set the name of the agent of this `askai` in
  the session and the separate repository for automatic transcripts. They
  correspond to `--agent` and `--transcripts` (section
  [Session](#session)).
- `upstream`, `service`, `testing` and `debugging` are objects that contain
  the options. The name of a member of one of these objects is the long
  name of an option without the two dashes: `"max-turns": 8` in `upstream`
  is `--max-turns 8` on the command line. The one exception is `name` in
  `service`, which is `--service-name`. Each option but `config`,
  `help`, the session options and the seven options of the built-in tools
  is a member of one of the four objects.
  Section [Members](#members) lists the options of each object, and
  `askai --help` lists them in a section for each object.
- `tools` can contain the objects `man`, `git`, `code`, `notes` and `jobs` of
  the built-in tools, whose members are the seven options of these tools
  (section [Built-in tools, in `tools`](#built-in-tools-in-tools)).
  `tools.git.timeout` is the member `timeout` of the object `git` in
  `tools`, and `"timeout": 60` there is `--git-timeout 60` on the command
  line. `tools` also allows or denies tools to clients and to the
  upstream model (section [Tools](#tools)).
- `roles`, `identities`, `unix-groups` and `anonymous` define who may
  connect (section [Who may connect](#who-may-connect)).
- `plugins` lists the plugins that `askai` loads (section
  [Plugins](#plugins)).

This file sets the upstream, one option of `service` and one of
`debugging`, and turns on the tool `man` with the object `man` in `tools`:

```json
{
  "upstream": { "url": "http://127.0.0.1:8081/v1/chat/completions" },
  "service": { "listen": "127.0.0.1:8090" },
  "debugging": { "verbose": true },
  "tools": { "man": { "enabled": true } }
}
```

If a member is in the wrong place, `askai` stops with exit status 2. The
message begins with the path of the file and names the member:

| Message | Cause |
| ------- | ----- |
| `url must be in upstream` | an option at the top level, or in another of the four objects than its own |
| `upstream: unknown member bogus` | a member of one of the four objects that is not an option, also `config` and `help` |
| `unknown member bogus` | an unknown member at the top level, also `config` and `help` |
| `upstream must be an object` | one of the four objects is not an object |
| `man must be tools.man.enabled` | an option of a built-in tool at the top level or in one of the four objects |
| `tools.git must be an object` | an object of a built-in tool in `tools` is not an object |
| `tools.git: unknown member paths` | a member of the object of a built-in tool that is not one of its options |

If two members of one object have the same name, `askai` uses the last
one.

A value has one of these forms:

- The value of an option with an argument is a string or a number. A number
  must be an integer, except for `temperature`: `"slots": 2.0` is refused.
- A size is an integer, or a string with a decimal integer that may end in
  `K`, `M` or `G` for KiB, MiB or GiB. The unit must be uppercase, and a
  size with a unit must be a string: `"max-body": "4M"` is 4194304 bytes.
- The value of an option without an argument is `true`. `false` changes
  nothing. Any other value is refused, also the string `"true"`.
- The value of a repeatable option (`header`, `allow-origin`, `listen`,
  `tools.git.repositories` and `tools.code.compile-commands`) is one value
  or an array of values.
- The value of `extra` is an object, or a string that contains a JSON object.
- `null` is refused as the value of an option.

JSON in the file may nest at most 32 levels, the object of the file
included. If a value nests deeper, `askai` stops with exit status 2 and the
message "nesting too deep".

### The file and the command line

`askai` applies the file first and the command line second. The two
combine as follows.

- An option with one value on the command line replaces the value of the
  file.
- `--header`, `--allow-origin`, `--git` and `--code` on the command line
  add to the lists of the file: `header`, `allow-origin`,
  `tools.git.repositories` and `tools.code.compile-commands`.
- Any `--listen` on the command line replaces the whole `listen` list of
  the file.
- `--extra` on the command line replaces the whole `extra` object of the
  file. `askai` does not merge the two objects.
- The command line cannot turn off an option without an argument that the
  file sets to `true`.
- `askai` reads only the last `--config` of the command line.
- Only the file can set `roles`, `identities`, `unix-groups`, `anonymous`
  and `plugins`, and the members of `tools` other than the objects of the
  built-in tools.

`askai` checks the options against each other after it has applied both.
For example, `max-body` must not exceed `body-budget`, also when the file
sets one of them and the command line sets the other.

### Reload with SIGHUP

With `--config` or a `tls:` listener, SIGHUP makes `askai` read the command
line, the file and the TLS files again. `askai` checks them as it does at
start, for example the git repositories, the notes directory, the API key
variable and the form of the listen addresses, with two exceptions: a
reload does not check `plugins`, and it does not open the listeners. So a
file that a reload accepts can still stop `askai` at the next start. If a
check fails, `askai` logs the error and changes nothing. Otherwise `askai`
applies these members:

- `roles`, `identities`, `unix-groups` and `anonymous`. The new policy
  applies to the requests that begin after the reload. The requests under
  way finish under the old policy. The reload also reads `/etc/subuid`
  and the kernel's overflow uid and gid again, and `/etc/subgid` if
  `unix-groups` names a group (section [Clients that askai refuses on a
  Unix socket](#clients-that-askai-refuses-on-a-unix-socket)). The new
  ranges apply to the connections that `askai` accepts after the reload.
- `tls-cert`, `tls-key` and `tls-allow-rsa` of `service`, if `askai`
  serves a `tls:` listener. New connections use the new certificate and
  key.

Every other member needs a restart, `plugins` and `tools` included, also
the objects of the built-in tools in `tools`. If another member has
changed, the reload logs the warning "other changed options need a
restart". After a reload, `tools` and `upstream` of a role can name only
the tools that the running `askai` offers. `askai` reads the variable of
an `env:` token from its own environment, and a reload does not change
that environment. So a new value of the variable, or a variable that was
not set when `askai` started, needs a restart.

Without `--config` and without a `tls:` listener, `askai` stops on SIGHUP
as it does on SIGTERM. If `askai` starts with SIGHUP ignored, as `nohup`
sets it, SIGHUP stays ignored without `--config` and without a `tls:`
listener. With either of them, `askai` reloads on SIGHUP in that case too.

### Permissions

`askai` refuses a configuration file that others than root and the user of
`askai` can change. The rules are these:

- The file must be a regular file of at most 1 MiB. The last component of
  its path must not be a symbolic link. A directory in the path may be a
  symbolic link, and `askai` then checks the directories of the real path.
- The owner of the file must be root or the user of `askai`, and neither
  the group nor others may have write permission.
- The same rule applies to every directory above the file. A directory
  that the group or others may write to is allowed if it has the sticky
  bit, as `/tmp` has.
- Others may read the file.

The files of `tls-cert` and `tls-key` must follow the same rules, and
others must not be able to read the file of `tls-key`. The rules of owner
and write permission also apply to the directory of `notes`, or to the
directory above it if it does not exist yet, to the files of `plugins`, and
to every directory above each of them. The path of a plugin must be the
real path of its file.

## What is on by default

### Which tools are on, without a file and with one

`askai` offers two sets of tools.

The first set is the tools of the clients. A client lists them with
`tools/list` and calls them with `tools/call`, as its role allows.

| Tool | `askai` offers it |
| ---- | ----------------- |
| `ask_ai`, or the name that `tool-name` sets | always |
| `man` | with `"enabled": true` in `tools.man` |
| `git` | with `repositories` in `tools.git` |
| `code` | with `compile-commands` in `tools.code`, when `askai-code` runs |
| `notes` | with `directory` in `tools.notes` |
| `test_task` | with `"test-task": true` in `testing` |
| `background`, `jobs`, `wait` and `timer` | with `"clients": true` in `tools.jobs` |
| the tools of the plugins | with `plugins` |

The second set is the tools of the upstream model. `askai` offers them
only with `"upstream-tools": true`, and only in the turns of an `ask_ai`
call (section [Tools for the upstream model, in
`upstream`](#tools-for-the-upstream-model-in-upstream)).
They are `man`, `git`, `code`, `notes` and the plugin tools whose kind has the
flag `TOOL_LENT` (`src/tool.h`), and `compact`, `findings`, `background`,
`jobs`, `wait` and `timer`. `compact` and `findings` never appear in
`tools/list`, and a role cannot name them. `background`, `jobs`, `wait` and
`timer` appear in `tools/list` only with `client-jobs`, as tools of the clients.
A role can then name them in `tools`, but never in `upstream`. The turns of an
`ask_ai` call get these four tools with a definition of their own, and their
jobs and timers are not those of the client.

Without any of these options, `askai` offers only `ask_ai`. In a file, the
objects of the built-in tools in the member `tools` turn tools on as their
options do, such as `"enabled": true` in `tools.man` for `--man`. The
other members and the other forms of `tools` turn no tool on. They can deny each of these tools but `ask_ai`, to clients,
to the upstream model, or to both (section [Tools](#tools)).

As `askai` accepts a connection on a Unix socket, it refuses the client
with the status 403 if the uid of the client is in a range of
`/etc/subuid`, is 4294967295, or is the kernel's overflow uid. This holds
under every policy (section [Clients that askai refuses on a Unix
socket](#clients-that-askai-refuses-on-a-unix-socket)).

The policy sets which client may call which of the tools that `tools` allows
clients. `askai` uses its own policy when there is no file, and when the
file has none of the members `roles`, `identities`, `unix-groups` and
`anonymous`:

- If the environment variable `ASKAI_TOKEN` is set and not empty, `askai`
  accepts only the clients that send `Authorization: Bearer TOKEN` with that
  value as TOKEN, also on a Unix socket. These clients may call every tool
  that `askai` offers, and they may read `GET /metrics`. The calls of a
  client on a Unix socket that sends the token remain the calls of its Unix
  user, for its notes, jobs and plugin instances.
- Otherwise `askai` accepts every client but one that sends a token on a
  Unix socket, and every client may call every tool that `askai` offers.
  Only clients on a Unix socket, and clients on loopback that send the
  `Host` `localhost`, `127.0.0.1` or `[::1]` with any port, or no `Host`,
  may read `GET /metrics`. For each TCP listener that is not on loopback,
  `askai` logs a warning that clients without a token may call tools there.
- The upstream model may call every tool that `askai` can offer to the
  upstream and that `tools` allows it.

Any one of the four members turns `askai`'s own policy off, also with the
value `null`. `askai` then ignores `ASKAI_TOKEN`, and logs a note about it.
Under a policy from the file, these rules apply:

- Over TCP, `askai` accepts a client only if the client sends the token of
  an identity, or if `anonymous` names a role for the way that the client
  connects. On a Unix socket, `askai` accepts a client only if the client
  sends no token and is a member of a group that `unix-groups` names, or if
  `anonymous` names a role for `unix`. A client on a Unix socket that sends
  a token gets the status 400. Other clients get the status 401.
- A client may list and call only the tools that its role lists in `tools`.
  A role without `tools` grants no tool.
- A client may read `GET /metrics` only if its role has `"metrics": true`.

So with `"enabled": true` in `tools.man` and a policy in the file, `askai`
offers `man` only to the clients whose role lists `man` in `tools`. With
a policy in the file, a client cannot list or call a tool that its role
does not list, even if `askai` offers the tool. With `roles` alone in the
file, `askai` refuses every client.

### How to turn on compaction

Compaction has no member of its own. `askai` offers the tool `compact` to
the upstream model in the turns of an `ask_ai` call when these conditions
are true:

1. `upstream-tools` is `true`.
2. The turns get at least one other tool that `tools` allows the upstream:
   `man`, `git`, `code` or a plugin tool that the role of the client allows the
   upstream to call, or `notes`, which the turns get in every call when
   `notes` is set.
3. The member `tools` allows `compact` to the upstream. Without `tools`,
   it does.

So the smallest file with compaction is this one:

```json
{
  "upstream": {
    "url": "http://127.0.0.1:8081/v1/chat/completions",
    "upstream-tools": true
  },
  "tools": {
    "notes": { "directory": "/var/lib/askai/notes" }
  }
}
```

`tools/list` does not list `compact`, because only the upstream model can
call it. The member `tools` of each upstream request lists it. In the
output of `GET /metrics`, the series
`askai_turn_tool_calls_total{tool="compact"}` counts the calls of
`compact`, and the series `askai_compactions_total` counts the compactions:
`by="upstream"` for a call of `compact`, and `by="askai"` for a full
context. The upstream must accept tools in the request. With
`llama-server`, its chat template must accept tools, as with `--jinja`.

The model calls `compact` with a summary of at most 4 MiB. `askai` then
removes the conversation after the prompt, and adds the summary, the review
record (section [The review record](#the-review-record)), the notices of
the agent and of the members of the session, the note history of the agent
and the newest note to the prompt. A newest note longer than a
tool result in a turn (section [`context`](#context)) is cut there, with the
offset from which the model reads on with `notes`.

`askai` also compacts the conversation without a summary when the context
is full. The context is full in these cases:

- From the second turn on, the upstream returns an error whose text
  contains `context size`, `context length` or `context window`. `askai`
  ignores case in this comparison. On the first turn, `askai` ends the
  `ask_ai` call with such an error.
- The context size is known, the upstream reports its token usage, and
  after the results of a turn the counted tokens reach the context size.
- The request grows past 16 MiB.

`askai` compacts in this way only once until the upstream answers again.
If the context is full again before that, the `ask_ai` call ends with an
error. `askai` counts the tokens from the usage that the upstream reports,
and at 4 bytes per token for the text that `askai` wrote after the last
answer. If the upstream reports no usage, `askai` does not compact by the
count, and it computes the notices below at 4 bytes per token for the whole
request.

The context size is the value of `context`, or else `n_ctx` from the
`/props` of `llama-server`. `askai` requests `/props` only if the upstream
can be `llama-server`: with `"provider": "llama"`, or with `url` and no
provider, and not with the APIs `generate` and `interactions`. With
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
again. If the context size is unknown, `askai` adds no notice, but the
model can still call `compact`.

### The review record

The turns of an `ask_ai` call keep a review record with the tool
`findings`. The turns get `findings` under the same conditions as
`compact`, and the member `tools` can deny it to the upstream. The record
lasts through all compactions of the `ask_ai` call. With `notes` in the
turns, a call whose turns added or updated findings saves them in the
notes repository before it answers, and the next `ask_ai` call with the
same `thread` starts with them, with an empty read log. If another call of
the thread saved first, the findings of this call are not saved, and
neither are those of a call that fails because the tool calls of one turn
take more than 16 MiB or because the context cannot hold the task and the
notes. Any other `ask_ai` call starts with an empty record. A background
job that resumes after a restart gets its record back from its
checkpoint, and `askai` makes the `findings` calls of the interrupted turn
again. The record contains these things:

- The findings of the model. `add` records a finding: a claim, an optional
  location such as `src/mcp.c:120-140`, and a state. A finding is a
  `candidate` until the model checks it. `update` changes the state to
  `verified`, `disproved`, `retracted` or `candidate`, with evidence, and
  can also change the claim or the location if they were wrong. The
  history of the finding contains each change with its turn, its evidence
  and the old claim or location.
- A log of the lines of committed files that the command `read` of the
  tool `git`, or the numbered lines of `describe` of the tool `code`,
  showed the turns whole, by repository, commit and path. The lines of a
  read in a background job count once `jobs` gives its result. A line that
  the 4 MiB limit of `git` cuts does not count. Only `read` and `describe`
  count: the output of `show`, `cat-file`, `grep` and `blame` does not.
  The log takes the lines of a result once the upstream has answered the
  request with the result, so the lines that a turn reads count from the
  next turn on. The lines of a result that a compaction removes before
  that do not count.

`show` gives the whole record, a window at a time from `offset`, or with
`id` the history of one finding.

A finding can be `verified` or `disproved` only if the lines of its
location are in the read log. Otherwise `askai` refuses the call, and the
answer names the `read` call that shows the lines, or the rest of them if
the log has the first of them. Without the tool `git`, it names no read
and says that only the lines that `code` `describe` shows count. If the
location goes past the end of the
file at each commit that the log has the file at, the answer gives the
number of lines of the file instead. If it goes past the end only at the
commit of the lines that the log has, the answer names a read at a commit
where the file may have them. If the record is too full to log the read,
the answer names no read. The lines of a read take the place of what the
log has of them, so the record takes the commit of a finding from the
latest read that showed its lines. The model does not give the commit. A
location can start with a commit ID of 7 to 64 hexadecimal digits and a
colon, as `git grep` prints it, and then only reads at that commit count;
`HEAD:` is as no commit. The path of a location can be short, such as
`mcp.c`, if exactly one path in the read log ends with `/mcp.c`. A
finding that got a short path before such a read gets the whole path in
its next `update`. If two paths of the log end with it by then, an
`update` to `verified` or `disproved` needs a `location` with the whole
path. Without the tools `git` and `code`, nothing fills the read log, so
`askai` refuses `verified` and `disproved`, and a finding stays a
`candidate` until the model retracts it. `askai` leaves its own notices out of
the end of a claim or of evidence, as it does for the text of a note.

After each compaction, `askai` adds the record to the prompt, after the
summary and before the notes. A line gives the number of findings and the
number in each state that has findings. The open findings come first, each
with its claim and its latest evidence, then a line that names the closed
states, then the closed findings, a line each, and then the read log.
There the record takes at most the window of a tool result in a turn
(section [`context`](#context)). If it is longer, its last line names the
`offset` from which `findings` `show` gives the rest.
The notice before the record states that `askai` keeps the task word for
word through every compaction: the system text, the messages of the client
and the prompt. So the model need not copy the task into its notes. The
description of `compact` states this too.

The record's limit is 4 MiB. `askai` counts the bytes of its texts, 64
bytes for each file and range of lines, and the size of its own structure
for each finding and change. `askai` refuses a call that would take the
record past 4 MiB, and the answer names the bytes of claim and evidence
that still fit, or states that the change does not fit. A read that does
not fit is not logged, and the record says from which turn on its log can
be incomplete. The series `askai_turn_tool_calls_total{tool="findings"}`
counts the calls of `findings`. To turn the record off, deny `findings` to
the upstream:

```json
{
  "tools": { "upstream": { "deny": ["findings"] } }
}
```

### What the role member upstream means

The member `upstream` of a role lists the tools that the upstream model may
call in the turns of the `ask_ai` calls of the clients of the role. It has
an effect only with `"upstream-tools": true`.

Without `upstream-tools`, the turns get no tool, and `upstream` must be
absent or `[]`. With a tool name in `upstream`, `askai` then stops with
exit status 2 and a message such as `role user: askai offers no tool "man"
to the upstream`.

With `upstream-tools`, the member has these meanings:

- If a role has no `upstream`, the upstream may call the tools in the
  `tools` of the role that `askai` can offer to the upstream: `man`, `git`,
  `code`, `notes` and the plugin tools whose kind has the flag `TOOL_LENT`.
  `askai` never offers `ask_ai` and `test_task` to the upstream.
- If `upstream` is `[]`, the upstream may call none of `man`, `git`, `code`
  and the plugin tools. With `notes`, the turns still get `notes`, `compact`,
  `findings`, `jobs`, `wait` and `timer`. Without `notes`, the turns get no
  tool, and an `ask_ai` call is one upstream request, as without
  `upstream-tools`.
- If `upstream` lists tools, the upstream may call these tools. Each name
  must be `man`, `git`, `code`, `notes` or a plugin tool whose kind has the flag
  `TOOL_LENT`, and `askai` must offer the tool in this run. For example,
  `"upstream": ["man"]` needs `"enabled": true` in `tools.man`. The list
  may name tools that the `tools` of the role does not list. The clients
  of the role then cannot call such a tool, but the upstream can call it
  for them.

In each case with `upstream-tools`, the turns also get these tools:

- `notes` in every call when `notes` is set, whatever the role lists;
- `compact`, `findings`, `jobs`, `wait` and `timer` when the turns get
  any tool;
- `background` when the turns get a tool that can run in the background:
  `man`, `git`, `code`, or a plugin tool whose kind has the flag `TOOL_DETACH`
  (`src/tool.h`).

The member `tools` applies before these rules: a tool that `tools` denies
to the upstream does not count in them. So the turns get `compact`,
`findings`, `jobs`, `wait` and `timer` only beside another tool that `tools`
allows, and `background` only beside such a tool that can run in the
background. The turns never get a tool that `tools` denies, whatever
`upstream` lists (section [Tools](#tools)).

Under `askai`'s own policy, the upstream may call every tool that `askai`
can offer to the upstream.

## Session

#### `agent`

The top-level member `agent` (`--agent NAME`) is the name of the agent of
this `askai` in the session of [`notes`](#notes). The agent is the AI member
of the session that answers the `ask_ai` calls of this `askai`. It is
required with `notes`, and `askai` stops with exit status 2 and the message
`--notes needs --agent NAME: the name of this askai's agent in the
session.` without it. Several `askai` services can run one agent.

The name is a member name: 1 to 64 characters from `[A-Za-z0-9_.@-]`. It
does not start with `.` or `-`, contains no `..`, does not end with `.` or
`.lock`, is not `@`, and is not a letter followed only by digits, such as
`c7`. Persons and agents share the names of a session.

As it starts, `askai` creates the session in the repository of `notes`:
the repository, if it does not exist, the session record
`refs/askai/session` with the ID, the name and the format of the session,
and the member record `refs/askai/members/NAME` of its agent. A member
record is a commit with the empty tree, whose message has the kind of the
member and the logins of a person. The first service of a name creates its
record, and a later service of an agent adopts it. If the record of the
name belongs to a person, `askai` stops with a message such as `agent bro:
the name bro belongs to the person with login uid 1000 in this session`. A
session of a later format than `askai` writes stops `askai` too. The
startup log lists the session, with its repository, ID, name, agent,
service and the number of notebooks of the old layout under
`refs/heads/notes/`, as `legacy`, if there are any, and the members that
the records name, each with its kind and logins.

A person becomes a member with the first write of `notes`, or with the
first ask call whose turns get `notes`: a Unix user of a Unix socket,
named like the user, or `uid-N` if the user has no such name; an
identity, named like the identity; or `ASKAI_TOKEN` over TCP under
`askai`'s own policy, which is the user who runs `askai`. A call that only
reads notes names the member of its login, and writes no record. An identity with
`"agent": true` is an agent. An identity and a Unix user of one name are one
person. A client over TCP without a token has no member name.

The top-level member `participant` (`--participant`) stops `askai` with exit
status 2 and the message `participant was replaced by agent and
service.name. Set agent to the name of the agent in the session. Set
service.name to the old participant value to keep the transcripts and the
background jobs.`

#### `transcripts`

The top-level member `transcripts` (`--transcripts PATH`) names a separate
bare Git repository for decoded upstream reasoning text. With
notes configured, its default is the notes path followed by
`.transcripts.git`; trailing slashes are removed first. Without notes,
`upstream.background: true` defaults to
`$XDG_STATE_HOME/askai/SERVICE/transcripts.git`, or
`$HOME/.local/state/askai/SERVICE/transcripts.git` when `XDG_STATE_HOME`
is not an absolute path, as the XDG Base Directory specification says;
`HOME` must then be absolute. SERVICE is the name of the service
([`service-name`](#service-name)). Without notes or background mode,
transcript storage is enabled by an explicit `transcripts` path.
Startup logs show the service and the resolved path. The transcript
repository must differ from the notes repository; the
notes tool does not expose it. Missing transcript path components are
created with mode 0700; symbolic links and `..` components are refused.
Two live instances cannot write to the same transcript repository with the
same service name: the second stops with the message `the service name
NAME is in use in this repository; give this askai another
--service-name`. Both options need a restart.

Thought deltas append to private plaintext files while reasoning is in
progress. The writer syncs and commits a file when reasoning ends, rather
than committing each event. Git contains only concatenated reasoning text
on `reasoning/SERVICE`; raw JSON, answer text and protocol envelopes
are not archived. Interrupted files remain available on disk.

Background recovery keeps bounded private checkpoints alongside the
repository, outside Git. Restoring a job now requires the provider to replay
its current turn from the beginning; saved thought prefixes are checked to
avoid duplicate appends. See [automatic transcripts](../README.md#automatic-transcripts)
for file locations, checkpoint limits and legacy archive handling. Existing
event archives are preserved and do not shrink automatically.

For two instances sharing one notes session:

```json
{
  "agent": "researcher",
  "transcripts": "/var/lib/askai/session-transcripts.git",
  "upstream": { "provider": "llama", "background": true },
  "tools": { "notes": { "directory": "/var/lib/askai/session-notes.git" } }
}
```

The second instance uses another `agent`, such as `reviewer`, and another
listen address. It can use the same transcript repository. Two instances of
one agent need different service names to share the transcript
repository.

#### Principals and their key

A principal tells whose calls they are: the owner of jobs, tasks and
timers, and of plugin instances, and the queue of the scheduler. `askai`
derives the principal of a Unix user from its uid, and the principal of an
identity from its name, with HMAC-SHA-256 and a secret key of 32 random
bytes. Bit 62 of a principal is set for an identity and clear for a Unix
user, and bit 63 is never set, so a Unix user and an identity never share a
principal. Notebooks are named after members, not after principals.

The key is the file `askai-journal/principal.key` in the transcript
repository, with mode 0600. `askai` makes it with random bytes at the
first start, while it holds the lock of the repository. Every service that
uses the same transcript repository reads the same
key, so a user has the same principal in all of them and across restarts.
Instances that share a notes directory share the key only if they also
share the transcript repository, which is the default. With notes, there
is always a transcript repository. Without one, `askai` stores nothing
that outlives the process, and uses a random key for each run. A key file
that does not have exactly 32 bytes, that has only zero bytes, or whose
mode lets other users access it stops `askai`. The message names the
file and the problem, such as `--transcripts: PATH:
askai-journal/principal.key: the file has 31 bytes, not 32`.

Principals from versions of `askai` without the key differ from the new
ones. Stored jobs made by those versions are no longer anyone's own.
Removing or replacing `principal.key` has the same effect.

## Members

### The upstream AI, in `upstream`

These options are members of the object `upstream`.

#### `provider`

The value is a string: `llama`, `openai`, `anthropic`, `google`,
`deepinfra` or `openrouter`. There is no default. Without a provider,
`url` is required.

The provider sets the defaults of `api`, `url` and `key-env`. The table in
section [Upstream providers and
APIs](../README.md#upstream-providers-and-apis) of `README.md` lists them.
`llama` is `llama-server` at `http://127.0.0.1:9931`.

#### `api`

The value is a string: `chat` (Chat Completions), `responses` (OpenAI
Responses), `messages` (Anthropic Messages), `generate` (Gemini
generateContent) or `interactions` (Gemini Interactions). The default is
the API of the provider: `responses` for `openai`, `messages` for
`anthropic`, `interactions` for `google`, and `chat` for the other
providers and without a provider. OpenAI refuses function tools in `chat`
for its reasoning models, so the turns of `upstream-tools` need
`responses` there. `google` also serves `generate` and `chat`.

`api` sets the format of the upstream requests and answers. `askai`
refuses an API that the provider does not serve, such as `"api":
"messages"` with `"provider": "openai"`.

#### `url`

The value is a string that begins with `http://` or `https://`. The
default is the URL of the provider for the API. Without a provider, there
is no default, and `url` is required.

`url` replaces the URL of the provider. With `url`, `askai` does not read
the key variables of the provider (see `key-env`). With the API
`generate`, if the path of the URL ends in `:generateContent`, `askai`
replaces that ending with `:streamGenerateContent`. `askai` also sets the
query parameter `alt` to `sse`, and does not change the other query
parameters.

#### `model`

The value is a string. There is no default. Without `model`, `askai`
sends no model name.

With a provider other than `llama` and without `url`, `model` is required.
With the API `generate`, `askai` sends no model name in the request body.
With `"provider": "google"`, the API `generate` and without `url`, `askai`
adds the model to the URL. With `url` and the API `generate`, the URL must
name the model, and `model` changes only the default of
`tool-description`.

#### `key-env`

The value is the name of an environment variable that contains the API
key. `askai` computes the default. At the URL of a provider, it is the
first variable of the provider that is set: `LLAMA_API_KEY` for `llama`,
`OPENAI_API_KEY` for `openai`, `ANTHROPIC_API_KEY` for `anthropic`,
`GEMINI_API_KEY` and then `GOOGLE_API_KEY` for `google`,
`DEEPINFRA_API_KEY` and then `DEEPINFRA_TOKEN` for `deepinfra`, and
`OPENROUTER_API_KEY` for `openrouter`. With `url`, or without a provider,
it is `ASKAI_API_KEY`.

`askai` sends the key in the key header of the API: `Authorization: Bearer
KEY`, or `x-api-key: KEY` for `messages`, or `x-goog-api-key: KEY` for
`generate` and `interactions`. If a `header` line has the same name, `askai`
sends that line instead of the key.

A variable that `key-env` names must be set and not empty, also when a
`header` line has the name of the key header. Without `key-env`, `askai`
reads no key variable if a `header` line has the name of the key header,
such as `Authorization` for `chat`. `askai` ignores case in this
comparison. Otherwise a missing key is an error at the URL of a provider
other than `llama`. With `url` or with `llama`, `askai` then sends no key.

#### `header`

The value is a header line, `"Name: value"`, or an array of header lines.
The default is no line. The command line adds to the lines of the file.

`askai` sends the lines in each upstream request. It also sends the header
of the API key, `anthropic-version: 2023-06-01` with the API `messages`,
and `User-Agent: askai/0.1`, each unless a line has that name. The name
must not be empty or contain blanks, and the line must not contain CR or LF.

#### `system`

The value is a string. The default is no system prompt.

`system` sets the default system prompt of an `ask_ai` call. The argument
`system` of a call replaces it. With an empty string, `askai` sends no
system prompt.

#### `temperature`

The value is a number from 0, or a string that contains such a number. It
may have a fraction. The default is none, and `askai` sends no
temperature.

`temperature` sets the sampling temperature of each upstream request.
`askai` refuses `NaN`, `Infinity` and a number that is too large for a
double, such as `1e400`.

#### `max-tokens`

The value is an integer from 1 to 2147483647. The default is none, and
`askai` sends no limit, except with the API `messages`, which requires one.
With `messages`, the default is 4096.

`max-tokens` sets the limit of output tokens of each upstream request.
`askai` marks an answer that the limit cuts off. A model's thinking is
output too, and it keeps `chunk-timeout` from ending a turn, so without
this limit a turn whose model thinks without end lasts until `timeout`.

#### `extra`

The value is a JSON object, or a string that contains one. The default is
none.

`askai` merges the object into the body of each upstream request,
recursively. For a member with the value `null`, `askai` removes that key
from the body. The numbers of the object must be finite: `askai` refuses
`NaN`, `Infinity` and a number that is too large for a double. `extra`
cannot turn off `"stream": true`. With `upstream-tools`, `extra` must not
contain the conversation of the API: `messages` for `chat` and `messages`,
`input` for `responses` and `interactions`, and `contents` for
`generate`.

OpenAI reasoning models require the request member `max_completion_tokens`
instead of `max_tokens`. With the API `chat`, `askai` sends `max_tokens`
only if `max-tokens` is set. Without `max-tokens`,
`{"max_completion_tokens": 4096}` is enough. With `max-tokens`,
`{"max_tokens": null, "max_completion_tokens": 4096}` removes `max_tokens`
and adds `max_completion_tokens`.

#### `background`

The value is a boolean, default `false`; the command-line option is
`--background`. When enabled, an ordinary ask call returns immediately
with a job ID. AskAI keeps the upstream stream alive independently of the
client connection. Call the same ask tool with `{"job": "ID"}` to check
for the result, or with `{"job": "ID", "action": "cancel"}` to cancel.
This needs no `client-jobs` setting or additional tool grant. The owner
and the original ask tool must match when looking up a job.

OpenAI Responses and Gemini Interactions also receive
`"background": true`, while other named provider/API combinations use
local detachment only. Selecting `responses` or `interactions` with a URL
and no provider requests native background execution too. Streaming stays
enabled. Native Gemini background requests default to `"store": true`,
which the provider requires for background interactions. OpenAI Responses
and synchronous Gemini requests keep the `"store": false` default.
`extra` may override `store`; explicitly forcing it to `false` can cause
Gemini to reject a background request.
Changing `background` needs a restart. See
[Asking without keeping the client connected](../README.md#asking-without-keeping-the-client-connected)
for the protocol, retention and cancellation details.

#### `timeout`

The value is an integer number of seconds from 0 to 2147483. The default
is 600. 0 means no limit.

`timeout` limits each upstream call twice: first the wait for an upstream
slot, then the call itself. Each turn of an `ask_ai` call is an upstream
call of its own.

#### `chunk-timeout`

The value is an integer number of seconds from 0 to 2147483. The default
is 60. 0 means no limit.

`chunk-timeout` limits the longest pause in an answer after the answer has
begun. After a longer pause, `askai` ends the call with an error.

#### `slots`

The value is an integer from 1 to 65535. `askai` computes the default:
`total_slots` from the `/props` of `llama-server`, or else 4.

`slots` sets how many upstream calls `askai` makes at once. The other
calls wait in a queue. `askai` requests `/props` only with `"provider":
"llama"`, or with `url` and no provider, and not with the APIs `generate`
and `interactions`.

#### `max-calls`

The value is an integer from 1 to 1000000. The default is 256.

`max-calls` limits the upstream calls that are queued or running. At most
8 calls can be queued for one principal: one identity, one Unix user, or
else one MCP session or client address. An `ask_ai` call past these limits
fails at once with an error, and the client can try again later.

### Tools for the upstream model, in `upstream`

These options are members of the object `upstream`. With
`"upstream-tools": true`, `askai` runs each `ask_ai` call as a loop of
turns. Each turn is an upstream call, and in its answer the model may call
tools. `askai` runs the calls, writes their results into the conversation,
and sends the next request. An answer that calls no tool ends the `ask_ai`
call, except in one case. If the answer has text, the turn is not the
last, and the context is not full, `askai` waits while a job of
`background` runs or a timer is set. When a job ends or a timer expires,
`askai` sends the next request, which reports the event to the model. If
such an event has not been reported to the model yet, `askai` sends the next
request at once. `askai` uses no upstream slot while it waits.

Besides `man`, `git`, `code` and the tools of plugins, the turns can get these
tools:

| Tool | What it does | When the turns get it |
| ---- | ------------ | --------------------- |
| `notes` | writes, lists and reads shared session notebooks | in every call with `notes` |
| `compact` | replaces the conversation after the prompt with a summary | with any other tool |
| `findings` | records findings and shows the call's review record of findings and of the lines read with `git` `read` | with any other tool |
| `background` | runs a call of a tool as a job, and returns the number of the job at once | with a tool that can run in the background |
| `jobs` | lists the jobs and the timers, returns the output of an ended job, or cancels a job or a timer | with any other tool |
| `wait` | waits up to 10 minutes for a job or a timer | with any other tool |
| `timer` | sets a timer of up to an hour, with a note | with any other tool |

Section [What the role member upstream
means](#what-the-role-member-upstream-means) describes which tools the turns
get. The section [Lending tools to the
upstream](../README.md#lending-tools-to-the-upstream) of `README.md`
describes the turns in detail.

#### `upstream-tools`

The value is `true`. The default is off.

With `upstream-tools`, `askai` offers tools to the upstream model in
`ask_ai` calls. If the turns of a call get no tool, the call is one
upstream request. The chat template of `llama-server` must accept tools,
as with `--jinja`. `upstream-tools` also changes the default of
`tool-description`.

#### `max-turns`

The value is an integer from 1 to 1000. The default is 32.

`max-turns` limits the upstream calls of one `ask_ai` call with
`upstream-tools`. Before the last turn, `askai` adds a notice that the next
turn is the last. If the last answer still calls tools, `askai` ends the
call, with an error if the answer has no text.

#### `context`

The value is an integer number of tokens from 0 to 2147483647. The default
is 0, and with 0 `askai` computes the size: `n_ctx` from the `/props` of
`llama-server`, or `inputTokenLimit` from `GET /v1beta/models/MODEL` of
Gemini's API with the APIs `generate` and `interactions` and a `model`, or
else unknown.

`context` sets the context size of the upstream model. `askai` uses it
with `upstream-tools` in three ways:

- A tool result in a turn is a window of an eighth of the context, counted
  at 4 bytes per token, but at least 2 KiB and at most 64 KiB. Without a
  known context size, the window is 64 KiB.
- `askai` adds the notices of section [How to turn on
  compaction](#how-to-turn-on-compaction).
- `askai` compacts the conversation when the counted tokens reach the
  context size.

### The ask tool, in `upstream`

These options are members of the object `upstream`. They set the name and
the description of the tool with which clients ask the upstream model.

#### `tool-name`

The value is a string of 1 to 128 characters from `A-Z`, `a-z`, `0-9`, `_`,
`.` and `-`. The default is `ask_ai`.

`tool-name` sets the name of the ask tool, and a role names the ask tool by
this name. The name must differ from the names of the other tools that
`askai` offers in this run. Several `askai` processes with different names
offer a model a choice, such as `ask_reasoner` and `ask_coder`.

#### `tool-description`

The value is a string. `askai` computes the default from `model`,
`upstream-tools` and `tools.notes.directory`:

> Send a question or a task to another AI model. The other AI cannot see
> this conversation, your files or your tools. Write a complete prompt. Use
> the other AI for a second opinion, for hard problems, or for knowledge you
> lack.

With `model` set to M, the first sentence ends with "to the AI model M". If
the turns of an `ask_ai` call can get a tool, `askai` adds "The other AI can
call some tools of this server. The result lists the tool calls after the
answer." after the third sentence. With an [`agent`](#agent) NAME, every
sentence names the agent instead:

> Send a question or a task to the agent NAME, on the AI model M. NAME
> cannot see this conversation, your files or your tools. Write a complete
> prompt. NAME can call some tools of this server. The result lists the
> tool calls after the answer. NAME keeps notes in the session. Each call
> starts with the notes of NAME. Ask NAME for a second opinion, for hard
> problems, or for knowledge you lack.

Without `model`, the first sentence ends after NAME. The sentences about
tools are there if the turns can get a tool, and the sentences about notes
if they can get `notes`. The turns can get a tool with `upstream-tools`,
when `askai` offers a tool that it can lend and `tools` allows the
upstream that tool.

### MCP server and listeners, in `service`

These options are members of the object `service`.

#### `service-name`

The member of the file is `service.name`. The value is a name of the same
form as [`agent`](#agent). The default is the name of the agent, or without
`agent` the name of the ask tool, `upstream.tool-name`. The name of the
service names this `askai` in its transcript repository, the directory
`askai-journal/SERVICE` and the branch `reasoning/SERVICE`, and it is the
committer of the records and of the notebook commits of the session. Keep
the name of a service the same across restarts to keep its transcripts and
recover its background jobs. Set it to the old value of `participant` to
keep the transcripts of an older `askai`. A name that `upstream.tool-name`
gives must have the form of a member name, or `askai` stops with exit status
2 and asks to set `service-name`.

#### `listen`

The value is a listen address, or an array of listen addresses. The
default is `["127.0.0.1:8090"]`. Any `--listen` on the command line
replaces the whole list of the file.

An address has one of these forms:

- `[host:]port` is a TCP address. Without a host, `askai` listens on
  `127.0.0.1`. With an empty host, as in `:8090`, `askai` listens on the
  first wildcard address that getaddrinfo() returns and that works. With
  glibc, that is `0.0.0.0`, for IPv4 only. With `[::]:8090`, `askai`
  listens on the IPv6 wildcard address, and also accepts IPv4 connections
  unless `net.ipv6.bindv6only` is 1. An IPv6 address is in brackets, as in
  `[::1]:8090`. For a host name, `askai` listens on the first of its
  addresses that works.
- `unix:PATH` is a Unix socket. PATH has 1 to 107 bytes. The mode of the
  socket file follows the umask of `askai`. `askai` replaces a socket file
  that no server listens on, and removes the file as it stops.
- `tls:[host:]port` is a TCP address with TLS 1.3 only, which needs
  `tls-cert` and `tls-key`. A client that offers ALPN must offer
  `http/1.1`. `askai` refuses `tls:unix:PATH` with exit status 2.

The port is an integer from 0 to 65535. `askai` checks the form of each
address at start, and stops with exit status 2 at an invalid address.
If `askai` cannot resolve a host name or listen on an address, it stops
with exit status 1.

#### `tls-cert`

The value is the path of a file. The default is none.

The file contains the certificate chain for the `tls:` listeners, in PEM:
the certificate of the server first, then the intermediate CA certificates.
`askai` sends the chain to clients as it is. `askai` reads the file only
with a `tls:` listener, and reads it again on a reload. The file must pass
the rules of section [Permissions](#permissions).

#### `tls-key`

The value is the path of a file. The default is none. A `tls:` listener
requires it.

The file contains the private key of the certificate, in PEM and not
encrypted: ECDSA on P-256, P-384 or P-521, or Ed25519. An RSA key needs
`tls-allow-rsa`. The key must match the certificate in `tls-cert`, or
`askai` stops with exit status 2. On a reload, `askai` logs this error and
changes nothing. The file must pass the rules of section
[Permissions](#permissions), and others must not be able to read it. It
may be the same file as that of `tls-cert`.

#### `tls-allow-rsa`

The value is `true`. The default is off.

`tls-allow-rsa` allows an RSA or RSA-PSS key in `tls-key`. An RSA
handshake uses about three times the CPU time of an ECDSA handshake.

#### `allow-origin`

The value is an Origin, or an array of Origins. The default is none. The
command line adds to the Origins of the file.

`askai` allows a browser request whose `Origin` field is exactly one of
these values, such as `https://example.com` without a slash at the end.
With `"*"`, `askai` allows any Origin. `askai` always allows loopback
Origins: `http` or `https` with the host `localhost`, `127.0.0.1` or
`[::1]` and any port. A request with another `Origin` gets the status 403.

#### `loops`

The value is an integer from 0 to 255. The default is 0, and with 0
`askai` computes the number: one fewer than the CPUs that `askai` may use,
as its CPU affinity and the CPU quota of its cgroup allow, at least 1 and
at most 255.

`loops` sets the number of event loops that serve clients.

#### `grace`

The value is an integer number of seconds from 1 to 3600. The default is
5.

`grace` limits the time to finish the requests under way after SIGINT,
SIGTERM, or SIGHUP without a reload. At the end of this time, `askai`
exits at once with status 3.

#### `memory-high`

The value is a size from 1 byte. `askai` computes the default: a sixteenth
of RAM, or of the lowest memory limit of its cgroups, but at least 256 MiB.

From this resident memory on, memory pressure is high, and from twice this
value on, it is critical. Under memory pressure, `askai` frees memory that
it has stored for reuse.

### Limits on clients, in `service`

These options are members of the object `service`. They limit what each
client may send and what `askai` stores for it. A client that exceeds a
limit gets an HTTP status, or `askai` closes the connection. A client
address is an IPv4 address, an IPv6 /64, or the user of a Unix socket
client.

#### `max-head`

The value is a size from 1 to 65535 bytes. The default is `"16K"`.

`max-head` limits the request line and the headers, and the trailer section
of a chunked body. A longer head or trailer section gets the status 431.

#### `max-fields`

The value is an integer from 0 to 4294967295. The default is 64.

`max-fields` limits the number of header fields, and the number of trailer
fields of a chunked body. More fields get the status 431. With 0, `askai`
refuses every request that has a header field.

#### `head-timeout`

The value is an integer number of seconds from 0 to 4294967295. The
default is 10. 0 means no limit.

`head-timeout` limits the time of a TLS handshake, and the time of a
request line and headers, each from its first byte. A late head gets the
status 408.

#### `max-body`

The value is a size. The default is `"8M"`, or `body-budget` or
`client-budget` if that is less. A value that is given must not exceed
`body-budget`, nor `client-budget` if that is set.

`max-body` limits a request body. A larger body gets the status 413. With
0, `askai` refuses every body. A file that a client hands `notes` inline
has at most 4 MiB, and less if `max-body` has no room for 4 MiB in base64
and 64 KiB of the rest of the request: `askai` advertises that size as
`maxInlineBytes`.

#### `min-body-rate`

The value is a size per second, from 0 to 4294967295 bytes. The default is
`"4K"`. 0 means no limit.

A request body must arrive at this average rate from the end of its
headers, once `body-grace` has passed. A slower body gets the status 408.

#### `body-grace`

The value is an integer number of seconds from 0 to 4294967295. The
default is 5.

`body-grace` sets the time after the headers before `min-body-rate`
applies.

#### `body-budget`

The value is a size. The default is `"256M"`. It must not be less than
`max-body`.

`body-budget` limits the request bodies in memory for all clients together.
A body past the budget gets the status 503 with `Retry-After: 1`.

#### `client-budget`

The value is a size from 1 byte. `askai` computes the default: a quarter
of `body-budget`, but at least `max-body`. It must not be less than
`max-body`.

`client-budget` limits the part of `body-budget` for one client address. A
body past it gets the status 503.

#### `max-depth`

The value is an integer from 1 to 2147483646. The default is 32.

`max-depth` limits the nesting of JSON in a request. A deeper request gets
the status 400.

#### `max-values`

The value is an integer from 1 to 4294967295. The default is 1024.

`max-values` limits the JSON values in a request, keys included. json-c
uses up to 0.7 KiB for each value while it parses. A request with more
values gets the status 400.

#### `max-members`

The value is an integer from 1 to 4294967294. The default is 1024.

`max-members` limits the members of one JSON object in a request. A larger
object gets the status 400.

#### `sse-keepalive`

The value is an integer number of seconds from 0 to 2147483647. The
default is 15. 0 means no comment lines.

`sse-keepalive` sets the time between the comment lines that `askai`
writes on an idle event stream.

#### `max-client-conns`

The value is an integer from 0 to 4294967295. The default is 256. 0 means
no limit.

`max-client-conns` limits the connections of one client address. Over the
limit, a plain HTTP connection gets the status 503, and `askai` closes a
TLS connection without an answer.

#### `idle-timeout`

The value is an integer number of seconds from 0 to 4294967295. The
default is 30. 0 means no limit.

`idle-timeout` limits the time that a connection may stay open with no
request under way, before its first request and between requests. After
that time, `askai` closes the connection.

#### `output-pause`

The value is a size from 1 byte. The default is `"256K"`. It must not
exceed `max-output`.

When this much output is pending on a connection, `askai` reads no more
input from it, and pauses a response until at most half of this is
pending.

#### `max-output`

The value is a size from 1 byte. The default is `"8M"`.

`askai` closes a connection if a response would make its pending output
larger than this size. Only a response that is all of the pending output
may pass this size, and only while `askai` has not paused it
(`output-pause`).

#### `send-timeout`

The value is an integer number of seconds from 0 to 4294967295. The
default is 30. 0 means no limit.

`send-timeout` limits the longest stall of pending output. If the output
stalls longer, `askai` closes the connection.

### Test aids, in `testing`

These options are members of the object `testing`. They exist for the
tests of `askai`.

#### `test-man-recheck`

The value is an integer number of seconds from 0 to 86400. The default is
600.

When the lookup of the file of a man page (`man -w`) is older than this,
`askai` repeats the lookup in the background at the next `man` call for the
page. With 0, `askai` repeats it at each call.

#### `test-stall`

The value is an integer number of milliseconds from 1 to 3600000. The
default is none. Without `test-stall`, `askai` does not handle SIGUSR2, so
the default action of the signal applies, and SIGUSR2 terminates `askai`.

With `test-stall`, on SIGUSR2 `askai` blocks each event loop in one turn
for this long.

#### `test-pressure`

The value is the path of a file. The default is none.

`askai` reads the file each second. Memory pressure is at least the level
in the file: 0 (none), 1 (high) or 2 (critical).

#### `test-fifo`

The value is `true`. The default is off.

With `test-fifo`, upstream calls wait in one queue for each class, first in
first out, and without the limit of 8 queued calls for one principal.

#### `test-task`

The value is `true`. The default is off.

`test-task` is a test aid. With it, `askai` offers the tool `test_task`,
which runs CPU work in slices of 250 µs as a task on the client loops, or,
with a prompt, an upstream call as background work.

### Debugging, in `debugging`

These options are members of the object `debugging`.

#### `verbose`

The value is `true`. The default is off.

Without `verbose`, `askai` logs as it starts the upstream, each tool and
whether it is on, each plugin with its tools, the tools of the upstream and
the policy, and it logs each tool call of a client that it refuses or that
fails. Section [Log](../README.md#log) of `README.md` has an example.

`verbose` makes `askai` also log each tool call of a client as it ends,
with its arguments cut at 200 bytes, and each ask call as it starts and
ends. It also logs each upstream turn, each tool call of a turn or of a
background job, each compaction, each notice about the context, each web
request of a plugin, and each man page that `askai` renders.

#### `watchdog`

The value is an integer number of milliseconds from 0 to 60000. The
default is 20. 0 means no report.

`askai` logs a turn of an event loop that is longer than this.

### Tools

The member `tools` is at the top level of the file, beside the four objects
of the options. It also contains the objects of the built-in tools
(section [Built-in tools, in `tools`](#built-in-tools-in-tools)).

#### `tools`

The value is `true`, `false`, a list of tool names, or an object. The
default is `true`.

`tools` allows or denies the tools that `askai` offers in this run, to
clients and to the upstream model. The forms in the table below and the
members `client` and `upstream` turn no tool on. The options, the objects
of the built-in tools and `plugins` turn tools on (section [Which tools are
on, without a file and with
one](#which-tools-are-on-without-a-file-and-with-one)). The ask tool is
always on, and `tools` cannot name it. The policy sets which clients may
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

An object of these forms can also contain the objects `man`, `git`,
`code`, `notes` and `jobs` of the built-in tools. The forms `true`, `false` and
a list cannot contain them. An object without `allow`, `deny`, `client` and
`upstream` allows all tools, as `{}` does, also when it contains objects
of the built-in tools.

Each of these forms applies to clients and to the upstream model. An
object with the members `client` and `upstream` sets the two apart. Each
of the two members has one of the forms of the table, and a missing member
allows all. The objects of the built-in tools are members of `tools`
itself, beside `client` and `upstream`, and never members of `client` or
`upstream`. With this file, clients get every tool, and the upstream model
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
than the ask tool: `man`, `git`, `code`, `notes`, `test_task`, `background`,
`jobs`, `wait`, `timer`, `compact`, `findings` or a tool of a plugin, each
only when `askai` offers it. `compact` and `findings` exist with
`upstream-tools`, and
`background`, `jobs`, `wait` and `timer` exist with `client-jobs` or
`upstream-tools`. Otherwise `askai` stops with exit status 2. The message
begins with the path of the file and `tools:`, or `tools: client:` or
`tools: upstream:` for those members:

| Message | Cause |
| ------- | ----- |
| `tools: askai has no tool "ls"; the tools are man, git` | a name of no tool, or of a tool that is off |
| `tools: ask_ai is the ask tool, which is always on` | the name of the ask tool |
| `tools: not a list of tools` | `allow` or `deny` is not an array |
| `tools: not true, false, a list or an object` | a value of another type |
| `tools: unknown member x` | a member other than `allow`, `deny`, `client`, `upstream`, `man`, `git`, `code`, `notes` and `jobs` |
| `tools: client: unknown member man` | a member of `client` or `upstream` other than `allow` and `deny`, also an object of a built-in tool |
| `tools: allow and deny together` | an object with both |
| `tools: allow and deny go into client and upstream` | `allow` or `deny` beside `client` or `upstream` |
| `tools: background needs jobs` | a form that allows `background` but not `jobs`, with which the output of a job is read |

For clients, `tools` has these effects:

- A client cannot list or call a tool that `tools` denies to clients,
  whatever its role grants. A role may name such a tool in `tools` or in
  `upstream` without an error. Under `askai`'s own policy, every client
  gets the tools that `tools` allows clients.
- What `tools` allows clients does not change the tools of the upstream.
  A role without `upstream` still lends the upstream the tools that its
  `tools` lists, also those that `tools` denies to clients.
- The warning "no role lists these tools" at start leaves out the tools
  that `tools` denies to clients.

For the upstream model, `tools` has these effects:

- The turns of an `ask_ai` call get only the tools that `tools` allows the
  upstream. This applies to every tool that the turns can get, also to
  `notes`, `compact`, `findings`, `background`, `jobs`, `wait` and
  `timer`. So with `"upstream": ["man"]`, the turns get `man` and no other
  tool: no compaction, no review record, no jobs and no timers.
- A denied tool does not count in the rules of section [What the role
  member upstream means](#what-the-role-member-upstream-means). With
  `{"deny": ["man"]}` and no other tool to lend, the turns get no
  `compact`, `findings`, `jobs`, `wait` or `timer`, and an `ask_ai` call is
  one upstream request.
- Without `notes` in the turns, an `ask_ai` call refuses the argument
  `thread`, and the ask tool does not list it. From two thirds of the
  context on, the notice to call `compact` comes only when the turns have
  `compact`.
- Without `upstream-tools`, the turns get no tool, whatever `tools`
  allows.

As it starts, `askai` logs a tool that `tools` denies to clients as `off
(tools)`, in the list `built-in tools for clients:` and after the name of
a tool of a plugin. The lists `upstream tools` of the policy show the
tools that the turns get. A change of `tools` needs a restart. A reload
checks the names of `tools` against the tools of the running `askai`.

### Built-in tools, in `tools`

These options are members of the objects `man`, `git`, `code`, `notes` and
`jobs` of the built-in tools in the member `tools`, with the names in this
table:

| Option | Member of the file |
| ------ | ------------------ |
| `--man` | `tools.man.enabled` |
| `--git PATH` | `tools.git.repositories` |
| `--git-timeout SECONDS` | `tools.git.timeout` |
| `--code REPO=FILE` | `tools.code.compile-commands` |
| `--code-unconfined` | `tools.code.unconfined` |
| `--notes DIR` | `tools.notes.directory` |
| `--client-jobs` | `tools.jobs.clients` |

`askai --help` shows these names in parentheses. The objects are members
of `tools` itself, beside its members `client` and `upstream`, or `allow`
or `deny`, so `tools` must be an object (section [Tools](#tools)). This
file turns on `man`, `git` with two repositories and `client-jobs`, and
denies `git` to the upstream model:

```json
{
  "upstream": {
    "url": "http://127.0.0.1:8081/v1/chat/completions",
    "upstream-tools": true
  },
  "tools": {
    "upstream": { "deny": ["git"] },
    "man": { "enabled": true },
    "git": {
      "repositories": ["/srv/git/proj", "/srv/git/lib.git"],
      "timeout": 60
    },
    "jobs": { "clients": true }
  }
}
```

The other members of `tools` allow or deny these tools. A change of an
object of a built-in tool needs a restart, as any change of `tools` does.

An option of a built-in tool in one of the four objects of the options, or
at the top level of the file, stops `askai` with exit status 2 and a
message such as `man must be tools.man.enabled` (section [Form](#form)).
For a value that `askai` refuses, the message names the option, such as
`invalid --git-timeout: 0`.

#### `man`

The member of the file is `tools.man.enabled`. The value is `true`. The
default is off.

With `man`, `askai` offers the tool `man`. The tool returns a page of the
manual as `man(1)` renders it, in plain UTF-8 text. `askai` renders each
page once and stores it. With `upstream-tools`, the upstream may call
`man` too, as the role of the client allows.

#### `git`

The member of the file is `tools.git.repositories`. The value is the path
of a repository, or an array of paths. The default is no repository. The
command line adds to the paths of the file.

With `git`, `askai` offers the tool `git`, which runs read-only git
commands over committed content in these repositories. Each path is the top
of a work tree or a bare repository. The tool names each repository by the last
component of its path, without `.git`: `/srv/git/proj.git` is `proj`. A name has 1 to
64 characters from `A-Z`, `a-z`, `0-9`, `_`, `.` and `-`, and does not
begin with a dot. `askai` refuses two repositories with the same name.
With `upstream-tools`, the upstream may call `git` too, as the role of the
client allows.

Use `help: true` with a command to get its allowed options, argument
limits and source selection without running it. Each line below is a
separate `git` tool call's arguments:

```jsonl
{"repo":"proj","command":"grep","help":true}
{"repo":"proj","command":"ls-tree","options":["-r","--name-only"]}
{"repo":"proj","command":"grep","options":["-F","-C3"],"pattern":"journal_write","paths":["src/"]}
{"repo":"proj","command":"read","revisions":["HEAD:src/journal.c"],"line":120,"count":40}
```

The default revision is `HEAD`, including for `grep`, `blame` and `ls-tree`.
Staged, working-tree, untracked and ignored content is not exposed. Use
`ls-tree` to list committed files; a staged deletion does not hide its committed
version. `status`, `ls-files`, `--cached`, `--staged` and `--untracked` are
unavailable; `log --all`, `--branches`, `--tags` and `--remotes` walk only
`HEAD` and the branches, tags and remote-tracking refs. `diff` requires two
committed revisions or a range
with both endpoints, such as `main..topic` or `main...topic`.

A `grep` pattern is an extended regex unless the options give `-G` (basic),
`-F` (literal) or `-P` (Perl); `-C3` gives three lines of context.
With one revision, the first line of the output names the commit:
`[Repository proj, commit ID]`. Each file path is on a line of its own, the
matches in the file follow as `line:text`, and an empty line separates the
files. With several revisions, the commit ID and a colon come before each
path. `--no-heading` gives `path:line:text` for each match.
Option values stay attached in one array entry. To read a committed file, use
`command: "read"` with `"revisions":["HEAD:src/journal.c"]`, and optionally
`line`, the first line (default 1), and `count`, the most lines (default as
many as fit in one result). Each line comes after its number and a tab. The
first line of the output names the full commit ID, and the last line gives the
revision and the `line` to read on from. The commit that a `grep` names and
the path of a match read as the `commit:path` revision, and the number of the
match as the `line`. `show` with the same
revision gives the file as it is. An immutable commit
ID can replace `HEAD` for repeatable reads. Index selectors such as
`":src/journal.c"`, raw blob or tree IDs, stash and reflog selectors, and
revision-list expansion syntax are rejected. Paths must be relative, without
`..` components or `:(...)` pathspec magic. Git replacement objects are ignored,
so they cannot substitute uncommitted content for a committed object. Path
names in the output are as they are (`core.quotePath=false`); only a name with
`"`, `\` or a control character is quoted.
Every requested commit, including `blame --ignore-rev` values, must be reachable
from `HEAD`, a branch, a tag or a remote-tracking ref. Private stash snapshots
and dangling commits are unavailable even by hash. Each call snapshots those
refs into a private bare view sharing the object storage; it imports no worktree,
index or local configuration, and preserves validated shallow boundaries.
Revisions are resolved to immutable IDs, and
attributes and mailmaps use the view's fixed `HEAD`. Repository metadata and
object storage remain trusted; this is not an operating-system sandbox.

Git output is captured up to 4 MiB. A result for an agent may take less;
`help: true` gives the most it takes as `window_bytes`, and the note of a
longer output gives the exact next `offset` to use with the same arguments.
`read` gives the whole lines that fit the window and pages by `line`
instead; only a line longer than the window is cut by bytes. A search
that writes no line says `[No matches.]`, with `-L`
`[Every file contains a match.]`, with `-v` `[Every line matches.]`, and
with both `[Every file contains a line that does not match.]`;
other nonzero Git exits are reported as errors. A revision that names no
readable commit gets one text that lists the causes, the same for a hash that
does not exist and one that is not reachable. A failed `read` or `cat-file` of
a `commit:path` gets only the note that the commit has no file at that path,
and other failed `show` and `cat-file` calls give a generic committed-object
error, so that Git's local-file existence hints cannot reveal uncommitted paths.

#### `git-timeout`

The member of the file is `tools.git.timeout`. The value is an integer
number of seconds from 1 to 3600. The default is 30.

The deadline spans history discovery, authorization and execution. At this
deadline, `askai` sends SIGTERM to the process group of `git`, and
SIGKILL one second later, and the call fails. A call of the tool `code`
uses the same deadline, at which `askai-code` is killed with SIGKILL.

#### `code`

The member of the file is `tools.code.compile-commands`. The value is a
string `REPO=FILE`, or an array of them. The default is none. The command
line adds to the values of the file.

With `code`, `askai` offers the tool `code`, which runs clang on the
committed C code of the repository `REPO` of `git`, with the flags of the
compile commands in `FILE`, a `compile_commands.json`. `REPO` is the name
of the repository, or an absolute path that is exactly one of the values
of `git`. A relative `FILE` is relative to the directory of the
repository. `askai-code`, the helper program in the directory of the
executable of `askai`, reads it at each call. Section [Asking clang about
C code](../README.md#asking-clang-about-c-code) of `README.md` describes
the tool.

`askai` stops with exit status 2 for a value without exactly one `=`, a
value whose `REPO` is not a repository of `git`, a value without `FILE`,
and for two values of one repository:

| Message | Cause |
| ------- | ----- |
| `--code: REPO=FILE needs the name or the path of a --git repository, one '=' and a file: nope=/x/compile_commands.json` | no repository of `git` is named `nope`; or `REPO` begins with `/` and is not a value of `git`; or the value has no `=` or more than one, or no `FILE` |
| `--code: two compile commands for proj` | two values are for `proj`, by its name or by its path |

At startup, `askai` runs `askai-code --check`. The tool is off, and the
startup report states the reason, if `askai-code` is not next to `askai` or
does not run, for example because its libclang is missing, if the kernel
does not support Landlock or `askai-code` contains no seccomp filter for
the architecture and `code-unconfined` is off, or if there is no cache
directory. The cache is `$XDG_CACHE_HOME/askai/code`, or
`$HOME/.cache/askai/code` if `XDG_CACHE_HOME` is not an absolute path. If
`HOME` is not an absolute path either, there is no cache directory.
`askai-code` creates the cache with mode 0700 and refuses to use it if
other users can access it. The cache contains one tree of a commit for each
repository. While the tool is off, a role that names `code` stops `askai`
with exit status 2, as a name of any tool that `askai` does not offer does.

#### `code-unconfined`

The member of the file is `tools.code.unconfined`. The value is `true`.
The default is off.

With `code-unconfined`, `askai-code` parses without Landlock and seccomp. It
still sets no_new_privs, limits its memory to 1 GiB and the size of the
files that it writes to 0, clears its environment and disables clang's
debug pragmas. A committed `#include` of an absolute path can then read any
file that the user of `askai` can read, such as `/proc/PID/environ` of the
user's processes, and committed code can make clang create empty files,
such as the temporary files of modules in `/tmp`. Use it only for
repositories whose code you trust.

#### `notes`

The member of the file is `tools.notes.directory`. The value is the path
of a directory. The default is none, and `askai` offers no tool `notes`.

With `notes`, `askai` offers the tool `notes` and uses this path as one bare
Git repository, or opens an existing ordinary repository or worktree.
Every instance configured with the same repository joins the same
session. If the directory does not exist, `askai` makes it with
mode 0700 when it starts. The directory above it must exist, or `askai`
stops with exit status 2. Clients get `notes` as their role allows. With
`upstream-tools`, the turns of every `ask_ai` call get `notes`.

Each member of the session (section [`agent`](#agent)) has one notebook,
named like the member, and writes only that notebook. The turns of an
`ask_ai` call write as the configured agent. A client writes as the
person or the agent of its login, and a client over TCP without a token
writes no notes. Every caller can read every notebook.
`{"action":"members"}` lists the members, each with its kind and the
newest change of its notebook, and says who the caller is; for a client,
the result also has `structuredContent` with the caller and the members.
`notebook` names the member whose notebook an action reads. The default is
the notebook of the caller. A name that no member has is refused with the
names of the members. Agents get who they are, who made the ask call and the
members of the session at the start of their prompt, and reads name the
source notebook separately from the note's contents.

`add` creates a new file and refuses an existing name. `replace` updates
an existing file in the caller's own notebook, adding a Git commit that
preserves its previous contents, and `append` adds text at the end of an
existing file, on a new line. `delete` removes an existing file; its
versions stay in the history. The message of each write's commit says
what the write did, the note's name and its size before and after, such
as `Replace "plan.md" (1234 to 1302 bytes)`. The result names the new
commit and repeats its message. If the write changed a note, the result also
names the commit of the previous version, after the lines that a `replace`
or `restore` removed and added. A `replace` or `restore` that would leave
the note's bytes as they are makes no commit; its result says `Unchanged`
and names the notebook's newest commit. If another call changes the
notebook during a write, askai reads the notebook again. If the note and
each directory in its name are as the write found them, askai makes the
same commit on top of the other change, with at most 3 tries in all.
Otherwise the write fails, and its result says to read the current note
and retry. No write overwrites another writer's changes.
When a turn writes text whose last lines are askai's own notices from tool
results, such as `[askai: notebook ...]` or the context notices, askai
leaves those lines out and says so in the result. Notices inside the
text, and text from clients, stay as they are. askai adds a line feed to
a turn's text that does not end with one. askai refuses a turn's text that
contains `</function>` and later `</tool_call>` or `<tool_call>`, and
writes nothing. The text argument of such a call ended early, and the next
tool call of the turn became part of the text. Text from clients is not
checked.
`search` lists the lines of the current notes that match `pattern`, whose
lines are extended regular expressions, case ignored.
`list` shows each current file once, with its byte count and blob identity;
`history` shows changes oldest first, with full commit IDs, times, the
lines added and removed by each change and commit messages.
`copy` adds a byte-for-byte copy to the caller's own notebook; its commit
message names the source notebook and the commit that it read. `copy`
needs `notebook`. With the APIs `generate`
and `interactions`, the model also gets the action `attach`, which uploads a
note to Gemini's Files API at the root of the API in the upstream's URL,
the part before `/v1beta/`, and gives it to the model as a file. A client
adds, appends or replaces a file that its program attaches, of at most
4 MiB, or less with a small `max-body`, with the argument `content`, by the
MCP extension `io.modelcontextprotocol/files` (section [Lending tools to the
upstream](../README.md#lending-tools-to-the-upstream) of `README.md`). The
directory and the directories above it must pass the rules of section
[Permissions](#permissions).

#### `client-jobs`

The member of the file is `tools.jobs.clients`. The value is `true`. The
default is off.

With `client-jobs`, `askai` offers clients the tools `background`, `jobs`,
`wait` and `timer`, as their role allows. `background` starts a call of
another tool of the client as a job, and answers at once with the job's id.
`jobs` lists the client's jobs and timers, returns the result of an ended
job, and cancels a job or a timer. `wait` waits up to 10 minutes for a job
to end or a timer to fire, and `timer` sets a timer of up to an hour. The
jobs and the timers belong to the Unix user of a client of a Unix socket,
to the identity of a client over TCP, or else to the client's address.
`askai` stores the result of a job for up to an hour after the job ends.
An owner runs at most 8 jobs and tasks at once, and has at most 8 timers
and 4 waits. A change of `client-jobs` needs a restart. The
section [Background jobs and timers for
clients](../README.md#background-jobs-and-timers-for-clients) of `README.md`
describes the tools in detail.

### Who may connect

Any of these four members replaces `askai`'s own policy with the policy of
the file (section [Which tools are on, without a file and with
one](#which-tools-are-on-without-a-file-and-with-one)). The names of roles
and identities have 1 to 64 characters from `A-Z`, `a-z`, `0-9`, `_`, `.`,
`@` and `-`.

#### `roles`

The value is an object. Each member is a role: its name, and an object
with the members `tools`, `upstream` and `metrics`. All three are
optional. There is no default.

- `tools` is an array of the tools that the clients of the role may list
  and call. The default is `[]`, no tool. Each name must be a tool that
  `askai` offers in this run: the ask tool, `man`, `git`, `code`, `notes`,
  `test_task`, `background`, `jobs`, `wait`, `timer` or a tool of a
  plugin. With any other name, `askai` stops with exit status 2, for
  example with `man` without `"enabled": true` in `tools.man`, or with
  `compact` or `findings`. A role may name a tool that the member `tools`
  denies to clients, but its clients do not get that tool.
- `upstream` is an array of the tools that the upstream model may call in
  the turns of the `ask_ai` calls of these clients. Section [What the role
  member upstream means](#what-the-role-member-upstream-means) describes
  it.
- `metrics` is `true` or `false`. The default is `false`. With `true`,
  the clients of the role may read `GET /metrics`.

#### `identities`

The value is an object. Each member is an identity: its name, and an
object with the members `role` and `tokens`, and optionally `agent`. There
is no default.

- `role` is the name of a role. It is required.
- `tokens` is an array of tokens, which must not be empty. `"sha256:HEX"`
  is the SHA-256 digest of the token in 64 hexadecimal digits, as `printf %s
  TOKEN | sha256sum` prints it, so the file does not contain the token.
  `"env:NAME"` names an environment variable of `askai` that contains the
  token. If the variable is not set or is empty, `askai` stops with exit
  status 2.
- `agent` is `true` or `false`. The default is `false`. With `true`, the
  identity is an agent of the session of [`notes`](#notes), such as an AI
  client program with a token of its own. Any other identity is a person.

An identity has no other members. A client over TCP authenticates as an
identity with `Authorization: Bearer TOKEN`. If the policy has any token,
the token that a client over TCP sends determines its identity, and an
unknown token gets the status 401. If the policy has no token, `askai`
ignores the tokens of the clients over TCP. No Unix user is an identity. A
client on a Unix socket that sends a token gets the status 400, also when
the token is valid, and a client there that sends no token gets its role
from `unix-groups` or `anonymous`. If the identities list the same token
twice, also within one identity, `askai` stops with exit status 2. All
calls of an identity share one queue of the scheduler.

#### `unix-groups`

The value is an object. Each member maps a Unix group, by its name or by
its gid as a string of digits, to the name of a role. `null` is refused.
`askai` looks up each group name when it reads the file. If no group has
the name, or the lookup fails, `askai` stops with exit status 2 at start.
There is no default. The entry of a group, with the names of its members,
may take up to 64 MiB. Give a very large directory group by its gid,
which `askai` does not look up.

A client on a Unix socket that sends no token gets the role of a group
that the client is a member of. If the client is
a member of several groups of `unix-groups`, `askai` uses the role of the
first of them in the file.

The kernel reports the user and the groups of the process that connected
to the socket, as they were when it called `connect()`. The user of the
client is its effective uid (`SO_PEERCRED`). The groups of the client are
its effective gid (`SO_PEERCRED`) and its supplementary groups
(`SO_PEERGROUPS`). After `newgrp`, or in a set-group-ID program, the
effective gid can differ from the primary group of the user.

A group in `unix-groups` may not have a gid in a range of `/etc/subgid`, or
the kernel's overflow gid (`/proc/sys/kernel/overflowgid`, usually 65534).
`askai` then stops with exit status 2 at start, and a reload keeps the old
policy. A process in a user namespace can have such a gid without being a
member of the group, so such a gid gives no role.

#### Clients that askai refuses on a Unix socket

`askai` refuses some clients on a Unix socket under every policy, its own
included. It answers them with the status 403 as soon as it accepts the
connection, before they count against `max-client-conns`:

- a client whose uid is in a range of `/etc/subuid`. The user that a
  range is delegated to can run processes with the uids of the range in a
  user namespace (`newuidmap`), so such a uid does not identify the user
  of the account that has the uid;
- uid 4294967295, which the kernel reports for a socket without
  credentials;
- the kernel's overflow uid (`/proc/sys/kernel/overflowuid`, usually
  65534). The kernel reports it for every uid that the user namespace of
  `askai` cannot map. So if 65534 is the overflow uid, a process that runs
  as `nobody` is refused.

`askai` reads `/etc/subuid` as it starts and on each reload, and
`/etc/subgid` only when `unix-groups` names a group. It does not read the
other sources that `/etc/nsswitch.conf` may name for `subid`. It reads
START and COUNT of a line `OWNER:START:COUNT` as `newuidmap` reads them:
decimal, hexadecimal after `0x`, or octal after `0`. A range that would
end above uid 4294967295 ends there. A missing file has no ranges. If a
file that `askai` reads exists but cannot be read, `askai` stops with exit
status 2 at start, and a reload keeps the old policy. If `askai` cannot read the overflow uid or gid, as with
systemd's `ProcSubset=pid`, it uses 65534 and logs a warning. The
startup report lists the uids that `askai` refuses under "Unix socket
peers refused", and every 5 seconds `askai` logs how many clients it
refused under "Unix socket peers turned away".

#### `anonymous`

The value is an object with the members `loopback`, `remote` and `unix`.
Each value is the name of a role or `null`. If a member is missing or
`null`, `askai` refuses those clients. There is no default.

`anonymous` sets the role of a client that sends no token, and of a client
over TCP that sends a token when no identity has `tokens`. The member `loopback`
applies to a client over TCP from a loopback address that sends the `Host`
`localhost`, `127.0.0.1` or `[::1]` with any port, or no `Host`. The member
`remote` applies to any other client over TCP. The member `unix` applies
to a user of a Unix socket that no group of `unix-groups` names.

If `remote` names a role with tools, `askai` logs a warning for each TCP
listener that is not on loopback, because clients without a token may call
tools there.

### Plugins

#### `plugins`

The value is an array. The default is no plugin. Each element is the
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

`askai` loads the plugins as it starts, in the order of the array. On a
reload, `askai` loads no plugin. The path must be the real path of the
file, without a symbolic link, and the file must pass the rules of section
[Permissions](#permissions). `askai` offers at most 128 tools in all, and
the names of the tools must differ.

`askai` refuses an element that is not an absolute path or an object, an
object without `path` or with another member than `path` and `options`,
`options` that is not an object, `options` for a plugin that has no
options, and options that the plugin refuses. If `askai` refuses a plugin,
it stops with exit status 2. The section [Plugins](../README.md#plugins)
of `README.md` describes plugins and the options of the godbolt plugin.

## The example file

[`askai.example.json`](askai.example.json) has every member that the file
can have. It sets every option, in the four objects and in the objects of
the built-in tools in the member `tools`, and it has a plugin, the members
`client` and `upstream` of `tools`, and a policy. So it shows the form of
each member in one file. The numbers and sizes are the defaults, except
`temperature`, `max-tokens`, `slots`, `context`, `memory-high` and
`test-stall`. The file turns on `upstream-tools`, `man`, `git`, `code`,
`notes` and `client-jobs`.

The file does not work as it is. Change or remove these members before
use:

| Member | Value in the file | What to do |
| ------ | ----------------- | ---------- |
| `provider`, `url` and `model` in `upstream` | `llama`, a `llama-server` at `127.0.0.1:8081`, `local` | set the upstream to ask |
| `key-env` in `upstream` | `"LLAMA_API_KEY"` | name a variable that is set and not empty, or remove the member; otherwise `askai` stops with exit status 2 |
| `listen` in `service` | `127.0.0.1:8090`, `tls:8443` and `unix:/run/askai/askai.sock` | set the addresses; the directory of a Unix socket must exist |
| `tls-cert` and `tls-key` in `service` | files in `/etc/askai` | set your files, which the listener `tls:8443` needs, or remove them with that listener |
| `allow-origin` in `service` | `"https://example.com"` | set the Origins of your browser clients, or remove the member |
| `test-stall` and `test-pressure` in `testing` | `100` and `"/run/askai/pressure"` | remove them: they are test aids |
| `path` of the plugin in `plugins` | `"/usr/libexec/askai/godbolt.so"` | set the real path of `godbolt.so`, or remove the plugin and the names of its tools in `tools` and `roles` |
| `repositories` in `tools.git` | `"/srv/git/proj"` | set repositories that exist |
| `compile-commands` in `tools.code` | `"proj=/srv/build/proj/compile_commands.json"` | name a repository of `tools.git`, or give its absolute path, and its compile commands, or remove the object |
| `agent` | `"bro"` | name the agent of the instance, which every member of the session sees |
| `name` in `service` | `"gpu1"` | choose a distinct stable name for each instance of the agent |
| `transcripts` | `"/var/lib/askai/transcripts.git"` | choose the separate transcript repository |
| `directory` in `tools.notes` | `"/var/lib/askai/notes.git"` | set the shared session repository path whose parent exists |
| `tokens` of the identities `ci` and `claude` | `"sha256:"` and 64 zeros, and `"sha256:"` and 64 ones | set the SHA-256 digest of a long random token; no token has the digest in the file |
| `unix-groups` | `{ "root": "admin", "users": "user" }` | set your groups, or remove the member; the first group of a client that the member names gives the client its role |

`test-fifo` and `test-task` are `false`, and `verbose` is `false`, so they
change nothing. `tls-allow-rsa` is `false` too.

The file has these effects on the tools and the policy:

- `tools` turns on `man`, `git`, `code`, `notes` and the tools of
  `client-jobs` with its objects of the built-in tools, allows clients
  every tool, and denies `godbolt_compile` to the upstream model. `code` is
  on only where `askai-code` runs.
- The role `admin` grants every tool but `code`, which no role lists, and
  `GET /metrics`. It has no `upstream`, so the upstream may call each tool of
  the role that `askai` can lend, but not `godbolt_compile`, which the member
  `tools` denies to the upstream.
- The role `user` grants `ask_ai`, `man` and `notes`. Its `upstream`
  allows the upstream to call `man`, `git`, `notes`, `godbolt_catalog` and
  `godbolt_targets`.
- The identities `ci` and `claude` have the role `user`. `claude` is an
  agent, such as an AI client program. On the Unix socket, the members
  of the Unix group `root` get the role `admin`, and the members of the
  Unix group `users` get the role `user`. A member of both groups gets
  `admin`, because `root` comes first.
- A client on loopback without a token gets the role `user`. `askai`
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
sends its prompt to the `llama-server` at `127.0.0.1:8081`. Every client
may call `ask_ai`, under `askai`'s own policy.

### A file that offers tools to the upstream model

```json
{
  "upstream": {
    "url": "http://127.0.0.1:8081/v1/chat/completions",
    "upstream-tools": true,
    "context": 131072
  },
  "tools": {
    "man": { "enabled": true },
    "git": { "repositories": ["/srv/git/proj.git"] },
    "notes": { "directory": "/var/lib/askai/notes" }
  }
}
```

Every client may call `ask_ai`, `man`, `git` and `notes`, under `askai`'s
own policy. The turns of each `ask_ai` call get `man`, `git`, `notes`,
`compact`, `findings`, `background`, `jobs`, `wait` and `timer`. `askai`
stores the notes that the model adds with the tool `notes` in the shared git
repository at `/var/lib/askai/notes`. With a context of 131072 tokens,
`askai` adds a reminder to write notes at 32768 and at 65536 tokens, and a
request to call `compact` from 87382 tokens on. A tool result in a turn is
a window of 64 KiB. The chat template of `llama-server` must accept tools,
as with `--jinja`.

### A policy

```json
{
  "upstream": {
    "url": "http://127.0.0.1:8081/v1/chat/completions",
    "upstream-tools": true
  },
  "service": {
    "listen": ["127.0.0.1:8090", "unix:/run/askai/askai.sock"]
  },
  "tools": {
    "man": { "enabled": true }
  },
  "roles": {
    "admin": { "tools": ["ask_ai", "man"], "metrics": true },
    "user": { "tools": ["ask_ai"], "upstream": ["man"] },
    "guest": { "tools": ["ask_ai"], "upstream": [] }
  },
  "identities": {
    "alice": { "role": "admin", "tokens": ["sha256:9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08"] },
    "ci": { "role": "user", "tokens": ["env:ASKAI_CI_TOKEN"] }
  },
  "unix-groups": { "developers": "user" },
  "anonymous": { "loopback": "guest", "remote": null, "unix": null }
}
```

The objects `upstream` and `service` set the upstream URL, turn on
`upstream-tools`, and set two listen addresses: `127.0.0.1:8090` and a
Unix socket. The object `man` in `tools` turns on `man`. The members of
the policy have these effects.

- The member `roles` turns `askai`'s own policy off. A client may then call
  only the tools of its role.
- The role `admin` allows its clients to call `ask_ai` and `man`, and to
  read `GET /metrics`. The role has no `upstream`, so in the turns of their
  `ask_ai` calls the upstream may call `man`, the one tool in the role's
  `tools` that `askai` can offer to the upstream. With `man`, the turns also get
  `compact`, `findings`, `background`, `jobs`, `wait` and `timer`.
- The role `user` allows its clients to call only `ask_ai`. The upstream
  may call `man` for them.
- The role `guest` allows its clients to call only `ask_ai`. The upstream
  may call no tool, so each `ask_ai` call of a guest is one upstream
  request.
- The identity `alice` has the role `admin`. A client that sends
  `Authorization: Bearer test` is `alice`, because `alice` lists the SHA-256
  digest of the token `test`. Use a long random token instead.
- The identity `ci` has the role `user`. A client that sends the token in
  the environment variable `ASKAI_CI_TOKEN` of `askai` is `ci`. `askai`
  does not start if the variable is not set.
- The member `unix-groups` gives the role `user` to a client on the Unix
  socket that is a member of the Unix group `developers` and sends no
  token. The group must exist.
- The member `anonymous` sets the role `guest` for a client on loopback
  without a token. `askai` refuses a client without a token over TCP from
  another address, and another user of the Unix socket.
