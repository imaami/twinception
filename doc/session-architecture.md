# Sessions, participants, and execution routes

Status: architectural direction, not a description of implemented behavior.
Recorded 2026-10-17. Near-term changes need not implement this design, but
should preserve the ability to evolve toward it.

## Required deployment flexibility

An employee's workstation may run one AskAI service with both a local GPU
model and a company LLM endpoint as upstreams. A terminal and a web UI may
connect to that service through Unix sockets and HTTP respectively.

The employee may also run a per-user AskAI proxy on a company Linux server,
accessed through SSH. That proxy connects to a system AskAI service through
a Unix socket; kernel peer credentials identify the employee's domain-mapped
Unix account. The system service may select an advanced company model and
also accept employees directly through HTTPS and JWT authentication.

These are required compositions, not exceptional deployments. Adding a
connection, proxy, service, or upstream must not implicitly create another
collaborator or notebook.

## Separate identity, participation, and execution

- A **user identity** identifies the authenticated person or service account.
  Unix credentials and verified network credentials may resolve to the same
  immutable identity. Connections, tokens, and mutable display names are
  not permanent user identifiers.
- A **session** is a durable shared workspace with an explicit membership
  policy and a portable persistent ID. It is independent of an HTTP/MCP
  session, a process, a repository pathname, and a conversation.
- A **participant** is a person or a logical agent taking part in a session.
  It has a stable ID and a readable name. An agent is a continuing
  collaborator, not an inference endpoint or an AskAI process.
- An **execution profile** selects an upstream, model, and allowed
  capabilities. A service can offer several profiles. A participant can
  change profiles without necessarily changing identity; distinct
  participants can use the same profile.
- A **conversation or task** scopes ongoing work within a session. Several
  conversations can share notes without sharing their entire context.
- A **run** records a particular execution, including the responsible user,
  participant, selected profile, and executing service. Concurrent runs and
  retries do not implicitly create notebooks.

A possible session roster is the employee, a researcher, a reviewer, and a
senior reviewer. The researcher might use the workstation GPU, the reviewer
the standard company endpoint, and the senior reviewer the central service.
This is an explicit collaboration arrangement, not a roster mechanically
derived from the available endpoints.

Opening a second UI or reconnecting preserves the selected participant and
conversation. Starting an independent collaborator is an explicit operation.
Keep service writer IDs for journal locking and recovery separate from
participant IDs and from user-visible authorship.

## Shared notes with author and task views

A session contains one shared collection of notes. A participant's notes are
a filtered view, not a storage partition exposed as a notebook address.
The current `participant/principal/{client,upstream}` namespace encodes
connection topology and should not become the permanent public contract.
`client`, `upstream`, `own`, and `other` are relative to a connection and do
not reliably describe authors through proxies or multiple upstreams.

Each note has an immutable unique ID, author, title, content, and optional
task/conversation and reply-to or supersedes references. Titles and original
attachment filenames need not be unique. Several people can write a
`plan.md`. Updates can append new notes referencing earlier versions,
preserving history rather than overwriting it.

The service binds the session and authorized writing participant to an
operation and stamps its author. A tool caller cannot impersonate another
participant merely by supplying an author field. Readers address notes by
ID and discover them by author, task, references, and incremental cursor.
For example, the eventual interface could support:

```text
notes list author=reviewer task=atomics
notes list since=<cursor>
notes read id=<note-id>
notes add title="Atomic initialization findings" text="..."
notes add reply_to=<note-id> text="I checked this against..."
```

These examples express intent, not a committed tool schema. Explicit
collections such as personal scratchpads or project findings can be added
with defined access rules if needed; they should not multiply with routes.
Addressing a reply does not by itself make its contents private.

## Large sessions and bounded model context

A shared session is not one giant conversation. A terminal and web UI can
open the same conversation or different conversations explicitly. Each
agent receives bounded context selected from its current task, summaries,
relevant notes, and changes since its last read. Joining or resuming a large
session must not require copying its entire history into a model prompt.

Discovery needs indexed filters and incremental cursors: what changed in a
task, what a reviewer concluded, which notes reply to a finding, and what
remains unresolved. Pagination, context budgets, and ordering should be
defined independently of the filesystem layout and host clocks.

## Forwarding and delegation

Forwarding preserves an existing operation's session, conversation,
participant, and operation IDs. A forwarding hop does not become the author
and does not duplicate notes. Stable operation IDs support retry
deduplication. Pure proxy mode must be distinguishable from asking a new
agent to perform work.

Delegation selects another participant and records the relationship to the
requesting task or run. For example, the researcher can ask the senior
reviewer to check a finding. Do not infer this distinction from whether an
upstream happens to be another AskAI service.

Every hop distinguishes the authenticated user authorizing work, the
participant performing it, and the service forwarding or executing it.
`SO_PEERCRED` authenticates the process at the Unix-socket hop; it does not
authenticate the original UI or prove arbitrary forwarded participant
claims. The receiver validates membership, authority to act for a
participant, and permission to use the requested execution profile.

Forwarded IDs are context, not credentials. Network delegation may use
scoped token exchange or another explicitly trusted mechanism. Neither
session membership nor a claimed participant may elevate model or tool
permissions. Secure the ingress of a per-user proxy as well as its egress.

## Storage and synchronization

Give a session a persistent ID and an explicit storage authority or location.
Hashing a local repository pathname cannot identify the same session across
machines. Matching user identities alone cannot reconcile independent stores.

The initial distributed design should prefer one authoritative store per
session, reachable through AskAI. That authority can run on a workstation
or centrally; it need not be one global company service. Local inference
can use remote session storage.

Git can remain the backend. The session store can serialize writes, handle
concurrent updates, and deduplicate retries. Branches used for storage or
concurrency remain implementation details rather than author addresses.

Offline replicas are a separate feature. Immutable notes with unique IDs
help synchronization, but membership changes, conflicting document
revisions, and concurrent writes still need explicit semantics. General
Git merging is not an initial distributed-session protocol.

## Evolution and compatibility

1. Introduce session and participant records independent of daemon
   configuration and authentication method.
2. Address notes by ID and expose author/task views.
3. Define request context carrying session, conversation, participant, and
   operation IDs alongside separately validated authorization.
4. Preserve that context through forwarding; make delegation explicit.
5. Support multiple execution profiles per AskAI service.
6. Preserve independent service writer IDs for journal locking and recovery.

Existing notes can be imported with their original service, principal, side,
file path, and source revision retained as provenance. Do not merge records
merely because their filenames or display names match. Preserve existing
data and ownership when changing identifier representations.

Until these features exist, avoid choices that make notebook ownership
depend permanently on sockets, connection counts, process names, upstream
count, or model names. A common user identity does not itself make quotas,
jobs, or conversations global; those are separate scope decisions.

## References

- [Linux Unix-socket credentials](https://man7.org/linux/man-pages/man7/unix.7.html)
- [OAuth 2.0 Token Exchange, RFC 8693](https://www.rfc-editor.org/rfc/rfc8693.html)
