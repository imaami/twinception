# twinception

Experimental C23 client for two llama.cpp `llama-server` instances. Both receive
the same user prompts. On each completed turn, model A's next history contains
model B's `reasoning_content`, and model B's contains model A's.

Default history mode is `split`:

- A remembers `answer_A + reasoning_B`.
- B remembers `answer_B + reasoning_A`.

`shared-a` and `shared-b` instead force the visible assistant content in both
histories to come from the selected model while retaining crossed reasoning.

## Dependencies

- C23 compiler
- libcurl
- json-c
- Linux epoll + timerfd

## Build

```sh
make
make check
```

## Server setup

Run two current llama.cpp servers on separate ports with thinking enabled. The
client requests `reasoning_format: "deepseek"` on every completion so extracted
thinking arrives in `delta.reasoning_content`.

Before the first inference request, twinception derives `/apply-template` from
each configured `/v1/chat/completions` URL and renders a synthetic history with
two `reasoning_content` markers. The newest historical marker must survive the
chat template or twinception refuses to run: without it, the advertised thought
swap would not actually reach the destination model.

The older marker is diagnostic. If it is pruned, twinception warns and continues:
the immediately preceding foreign trace is still injected, but earlier traces are
not retained by the rendered history. For templates that support it, start
`llama-server` with `--reasoning-preserve` to retain the complete crossed
reasoning history.

Example shape:

```sh
llama-server -m /path/to/gemma.gguf --host 127.0.0.1 --port 8080 -ngl 99
llama-server -m /path/to/other.gguf --host 127.0.0.1 --port 8081 -ngl 0
```

Use the model-specific options you would normally use for context size, threads,
GPU offload, and reasoning budget. The client always supplies the crossed
`reasoning_content` fields; the startup template check verifies what the server
actually serializes into the model prompt.

## Use

```sh
./twinception -d
```

The defaults are:

- A: `http://127.0.0.1:8080/v1/chat/completions`
- B: `http://127.0.0.1:8081/v1/chat/completions`
- history mode: `split`

For a single prompt:

```sh
./twinception -d -p 'Prove or disprove that every tree is bipartite.'
```

For router servers or aliases, add `-A NAME` and `-B NAME`. The template check
expects each URL to end in `/v1/chat/completions`; use `-P` /
`--no-template-check` only when a proxy or non-llama backend makes the probe
unavailable and you have verified its template behavior separately.

`:quit` exits the interactive client after any in-flight paired turn completes.

A paired turn is committed only if both requests finish successfully. A failure
on either backend leaves both histories unchanged.
