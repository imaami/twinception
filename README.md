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

Example shape:

```sh
llama-server -m /path/to/gemma.gguf --host 127.0.0.1 --port 8080 -ngl 99
llama-server -m /path/to/other.gguf --host 127.0.0.1 --port 8081 -ngl 0
```

Use the model-specific options you would normally use for context size, threads,
GPU offload, and reasoning budget. The client always supplies the crossed
`reasoning_content` fields. Whether older reasoning traces remain in the
serialized prompt is still chat-template policy; on templates that support it,
llama.cpp's `--reasoning-preserve` makes that full-history behavior explicit.
Leaving it at the template default is also a useful control condition.

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

For router servers or aliases, add `-A NAME` and `-B NAME`.

`:quit` exits the interactive client after any in-flight paired turn completes.

A paired turn is committed only if both requests finish successfully. A failure
on either backend leaves both histories unchanged.
