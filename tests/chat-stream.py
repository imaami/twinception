#!/usr/bin/env python3
"""Opt-in live AskAI deltas, immutable mailbox flow control and persistence."""

import json
import os
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

from askai import (ANSWERS, API, Askai, Stream, Upstream, sse_of, sse_wire,
                   text_of, turn_stream, wait_for)


class Gate(Stream):
    """Hold the provider's terminal events until its first delta reaches MCP."""

    def __init__(self, first, last):
        super().__init__(wire=first)
        self.last = last
        self.release = threading.Event()

    def pieces(self):
        yield self.wire
        assert self.release.wait(15), "MCP did not deliver a live delta"
        yield self.last


def begin(ai, rpc_id=41, progress=False, receive_buffer=None):
    conn = ai.connect()
    if receive_buffer:
        conn.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        conn.sock.settimeout(conn.timeout)
        conn.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, receive_buffer)
        conn.sock.connect((ai.host, ai.port))
    meta = {"askai": {"stream": True}}
    if progress:
        meta["progressToken"] = rpc_id
    conn.request("POST", "/mcp", json.dumps({
        "jsonrpc": "2.0", "id": rpc_id, "method": "tools/call", "params": {
            "name": "ask_ai", "arguments": {"prompt": "Q", "background": False},
            "_meta": meta}}), headers={
                "Content-Type": "application/json", "Accept": "text/event-stream"})
    response = conn.getresponse()
    assert response.status == 200 and response.getheader("Content-Type") == \
        "text/event-stream", (response.status, response.read())
    return conn, response


def event(response):
    while line := response.readline():
        if line.startswith(b"data: "):
            return json.loads(line[6:])
    return None


def finish(response, seen=()):
    messages = list(seen)
    while item := event(response):
        messages.append(item)
    return messages


def deltas(messages, kind="text"):
    return [m["params"] for m in messages
            if m.get("method") == "notifications/askai/delta" and
            m["params"]["kind"] == kind]


def check_apis(binary, up):
    paths = {"openai": "/v1/chat/completions", "responses": "/v1/responses",
             "anthropic": "/v1/messages", "gemini": "/v1beta/models/m:generateContent",
             "interactions": "/v1beta/interactions"}
    for protocol, api in API.items():
        answer = ANSWERS[protocol]
        if protocol == "responses":
            answer = {"status": "completed", "output": [
                {"type": "reasoning", "content": [{"text": "hmm"}]},
                {"type": "message", "content": [{"type": "output_text", "text": "A"}]}]}
        events = sse_of(answer)
        terminal = 2 if protocol in ("openai", "anthropic", "interactions") else 1
        gate = Gate(sse_wire(events[:-terminal]), sse_wire(events[-terminal:]))
        up.reply = lambda request, g=gate: (200, g)
        try:
            with Askai(binary, "--api", api, "-u", up.url(paths[protocol]), "-m", "m") as ai:
                conn, response = begin(ai, 'id-"ää')
                first = event(response)
                assert first["method"] == "notifications/askai/delta", (protocol, first)
                gate.release.set()
                messages = finish(response, [first])
                conn.close()
                assert "".join(d["text"] for d in deltas(messages)) == "A", (protocol, messages)
                assert "".join(d["text"] for d in deltas(messages, "reasoning")) == "hmm", \
                    (protocol, messages)
                assert all(d["requestId"] == 'id-"ää' and d["turn"] == 1
                           for kind in ("text", "reasoning") for d in deltas(messages, kind))
                assert text_of(messages[-1]["result"]) == "A", messages[-1]
                # Existing clients do not receive the optional extension.
                assert text_of(ai.ask()) == "A"
        finally:
            gate.release.set()
    print("chat stream: all five APIs deliver text/reasoning before completion: PASS")


def check_large(binary, up):
    text = 'ää😀 "\\\n' * 70_000
    wire = sse_wire([(None, {"choices": [{"index": 0, "delta": {"content": text}}]}),
                    (None, "[DONE]")])
    up.reply = lambda request: (200, Stream(wire=wire))
    with Askai(binary, "-u", up.url()) as ai:
        conn, response = begin(ai)
        messages = finish(response)
        conn.close()
        assert "".join(d["text"] for d in deltas(messages)) == text
        assert all(0 < len(d["text"].encode()) <= 8192 for d in deltas(messages))
        assert text_of(messages[-1]["result"]) == text
    print("chat stream: large events split on UTF-8 boundaries without duplicate text: PASS")


def check_tools(binary, up):
    with tempfile.TemporaryDirectory() as directory:
        repo = os.path.join(directory, "project")
        subprocess.run(["git", "init", "-q", repo], check=True)
        subprocess.run(["git", "-C", repo, "-c", "user.name=t", "-c", "user.email=t@t",
                        "commit", "-q", "--allow-empty", "-m", "first"], check=True)
        replies = [turn_stream([("call1", "git", json.dumps({"repo": "project",
                    "command": "log", "options": ["--oneline"]}))],
                    text="Checking.", reasoning="Need log."), turn_stream(text="Done.")]
        up.reply = lambda request: (200, replies.pop(0))
        archive = os.path.join(directory, "transcripts.git")
        with Askai(binary, "-u", up.url(), "--git", repo, "--upstream-tools",
                   "--transcripts", archive, "--participant", "tools") as ai:
            conn, response = begin(ai, progress=True)
            messages = finish(response)
            conn.close()
            assert "".join(d["text"] for d in deltas(messages) if d["turn"] == 1) == "Checking."
            assert "".join(d["text"] for d in deltas(messages) if d["turn"] == 2) == "Done."
            assert messages[-1]["result"]["content"][0]["text"] == "Done."
            assert "first" in up.last()["raw"].decode() and not replies
            progress = [m["params"] for m in messages
                        if m.get("method") == "notifications/progress"]
            assert progress and all(p["progressToken"] == 41 for p in progress)
            assert all(p["_meta"]["askai"]["kind"] ==
                       ("tool" if "calling " in p["message"] else "status")
                       for p in progress), progress
            assert any(p["_meta"]["askai"]["kind"] == "tool" and
                       p["message"] == "turn 1, calling git" for p in progress), progress
            assert any(p["_meta"]["askai"]["kind"] == "status"
                       for p in progress), progress
            assert list(reasoning_tree(archive, "tools").values()) == [b"Need log."]
    print("chat stream: tool execution continues through distinct streamed turns: PASS")


def reasoning_files(repo, participant):
    return sorted(Path(repo, "askai-journal", participant, "reasoning").rglob("*.txt"))


def reasoning_tree(repo, participant):
    ref = f"refs/heads/reasoning/{participant}"
    names = subprocess.run(["git", "--git-dir", repo, "ls-tree", "-r", "--name-only", ref],
                           capture_output=True, text=True)
    if names.returncode:
        return {}
    return {name: subprocess.run(["git", "--git-dir", repo, "show", f"{ref}:{name}"],
                                 check=True, capture_output=True).stdout
            for name in names.stdout.splitlines()}


def reasoning_answer(protocol, thought, text="answer stays out of Git"):
    answer = json.loads(json.dumps(ANSWERS[protocol]))
    if protocol == "openai":
        answer["choices"][0]["message"].update(reasoning_content=thought, content=text)
    elif protocol == "responses":
        answer["output"] = [
            {"type": "reasoning", "content": [{"text": thought}]},
            {"type": "message", "content": [{"type": "output_text", "text": text}]}]
    elif protocol == "anthropic":
        answer["content"] = [{"type": "thinking", "thinking": thought},
                             {"type": "text", "text": text}]
    elif protocol == "gemini":
        answer["candidates"][0]["content"]["parts"] = [
            {"text": thought, "thought": True}, {"text": text}]
    else:
        answer["steps"] = [{"type": "thought", "summary": [{"type": "text", "text": thought}]},
                           {"type": "model_output", "content": [{"type": "text", "text": text}]}]
    return answer


def first_answer(protocol, item):
    if protocol == "openai":
        return bool(item.get("choices", [{}])[0].get("delta", {}).get("content"))
    if protocol == "responses":
        return item.get("type") == "response.output_text.delta"
    if protocol == "anthropic":
        return item.get("delta", {}).get("type") == "text_delta"
    if protocol == "interactions":
        return item.get("delta", {}).get("type") == "text"
    return any(part.get("text") and not part.get("thought")
               for candidate in item.get("candidates", [])
               for part in candidate.get("content", {}).get("parts", []))


def check_journal(binary, up):
    thought = 'decoded \"thought\" \\ ää 😀\nsecond line\tend'
    paths = {"openai": "/v1/chat/completions", "responses": "/v1/responses",
             "anthropic": "/v1/messages", "gemini": "/v1beta/models/m:generateContent",
             "interactions": "/v1beta/interactions"}

    class Phases(Stream):
        def __init__(self, events, split, terminal):
            super().__init__(wire=b"")
            self.wires = (sse_wire(events[:split]), sse_wire(events[split:-terminal]),
                          sse_wire(events[-terminal:]))
            self.reasoning_done = threading.Event()
            self.answer_done = threading.Event()

        def pieces(self):
            yield self.wires[0]
            assert self.reasoning_done.wait(15), "reasoning archive was not inspected"
            yield self.wires[1]
            assert self.answer_done.wait(15), "completed reasoning was not committed"
            yield self.wires[2]

    for protocol, api in API.items():
        answer = reasoning_answer(protocol, thought)
        events = sse_of(answer)
        split = next(i for i, (_, item) in enumerate(events)
                     if isinstance(item, dict) and first_answer(protocol, item))
        if protocol in ("anthropic", "interactions"):
            stop = "content_block_stop" if protocol == "anthropic" else "step.stop"
            split = next(i for i, (name, _) in enumerate(events) if name == stop)
        terminal = 2 if protocol in ("openai", "anthropic", "interactions") else 1
        stream = Phases(events, split, terminal)
        up.reply = lambda request: (200, stream)
        with tempfile.TemporaryDirectory() as directory:
            repo = os.path.join(directory, "transcripts.git")
            try:
                with Askai(binary, "--api", api, "-u", up.url(paths[protocol]), "-m", "m",
                           "--transcripts", repo, "--participant", "archive") as ai:
                    conn, response = begin(ai)
                    seen = []
                    while "".join(d["text"] for d in deltas(seen, "reasoning")) != thought:
                        item = event(response)
                        assert item is not None, (protocol, seen)
                        seen.append(item)
                    def appended():
                        files = reasoning_files(repo, "archive")
                        return len(files) == 1 and files[0].read_bytes() == thought.encode()
                    wait_for(appended)
                    assert not reasoning_tree(repo, "archive"), protocol
                    stream.reasoning_done.set()
                    while not deltas(seen):
                        item = event(response)
                        assert item is not None, (protocol, seen)
                        seen.append(item)
                    wait_for(lambda: list(reasoning_tree(repo, "archive").values()) ==
                             [thought.encode()])
                    stream.answer_done.set()
                    messages = finish(response, seen)
                    conn.close()
                    assert text_of(messages[-1]["result"]) == "answer stays out of Git", messages[-1]
                    assert all(name.endswith(".txt") for name in reasoning_tree(repo, "archive"))
                    commits = subprocess.run(["git", "--git-dir", repo, "rev-list", "--count",
                                              "refs/heads/reasoning/archive"],
                                             check=True, capture_output=True, text=True)
                    assert commits.stdout.strip() == "1", (protocol, commits.stdout)
                    # A non-streaming provider still exposes decoded reasoning.
                    up.plain = True
                    up.reply = lambda request, a=answer: (200, a)
                    assert text_of(ai.ask()) == "answer stays out of Git"
                    wait_for(lambda: sorted(reasoning_tree(repo, "archive").values()) ==
                             [thought.encode(), thought.encode()])
                    # Answer-only requests must not create empty thought files.
                    up.reply = lambda request, p=protocol: (200, reasoning_answer(p, ""))
                    assert text_of(ai.ask()) == "answer stays out of Git"
                    assert len(reasoning_files(repo, "archive")) == 2
                    assert len(reasoning_tree(repo, "archive")) == 2
            finally:
                up.plain = False
                stream.reasoning_done.set()
                stream.answer_done.set()
            subprocess.run(["git", "--git-dir", repo, "fsck", "--full", "--strict"],
                           check=True, capture_output=True)
    print("reasoning archive: five APIs append decoded text, commit at answer boundary, "
          "and capture plain responses without raw events or answers: PASS")


def check_journal_stops(binary, up):
    """Explicit block completion commits thoughts while the answer is held.
    Responses text/summary part completion is not a whole reasoning item."""
    class Stopped(Stream):
        def __init__(self, first, stop, last):
            super().__init__(wire=first)
            self.stop, self.last = stop, last
            self.allow_stop, self.allow_answer = threading.Event(), threading.Event()

        def pieces(self):
            yield self.wire
            assert self.allow_stop.wait(15), "open reasoning was not inspected"
            yield self.stop
            assert self.allow_answer.wait(15), "closed reasoning was not committed"
            yield self.last

    for protocol, path in (("responses", "/v1/responses"),
                           ("anthropic", "/v1/messages"),
                           ("interactions", "/v1beta/interactions")):
        thought = "first part, second part"
        events = sse_of(reasoning_answer(protocol, thought, "A"))
        if protocol == "responses":
            split = next(i for i, (_, item) in enumerate(events)
                         if first_answer(protocol, item))
            first = [("response.created", {"type": "response.created"}),
                     ("response.reasoning_text.delta", {"type": "response.reasoning_text.delta",
                                                         "delta": "first part, "}),
                     ("response.reasoning_text.done", {"type": "response.reasoning_text.done"}),
                     ("response.reasoning_summary_part.done", {"type": "response.reasoning_summary_part.done"}),
                     ("response.reasoning_text.delta", {"type": "response.reasoning_text.delta",
                                                         "delta": "second part"})]
            stop = [("response.output_item.done", {"type": "response.output_item.done",
                                                   "item": {"type": "reasoning"}})]
            last = events[split:]
        else:
            name = "content_block_stop" if protocol == "anthropic" else "step.stop"
            split = next(i for i, (kind, _) in enumerate(events) if kind == name)
            first, stop, last = events[:split], events[split:split + 1], events[split + 1:]
        stream = Stopped(sse_wire(first), sse_wire(stop), sse_wire(last))
        up.reply = lambda request: (200, stream)
        with tempfile.TemporaryDirectory() as directory:
            repo = os.path.join(directory, "transcripts.git")
            try:
                with Askai(binary, "-u", up.url(path), "--api", API[protocol],
                           "--transcripts", repo, "--participant", "stops") as ai:
                    conn, response = begin(ai)
                    seen = []
                    while "".join(d["text"] for d in deltas(seen, "reasoning")) != thought:
                        item = event(response)
                        assert item is not None, (protocol, seen)
                        seen.append(item)
                    wait_for(lambda: len(reasoning_files(repo, "stops")) == 1 and
                             reasoning_files(repo, "stops")[0].read_bytes() == thought.encode())
                    assert not reasoning_tree(repo, "stops"), protocol
                    stream.allow_stop.set()
                    wait_for(lambda: list(reasoning_tree(repo, "stops").values()) == [thought.encode()])
                    assert not deltas(seen), seen
                    stream.allow_answer.set()
                    messages = finish(response, seen)
                    conn.close()
                    assert text_of(messages[-1]["result"]) == "A", messages[-1]
            finally:
                stream.allow_stop.set()
                stream.allow_answer.set()
    print("reasoning archive: explicit item/block/step stops commit before a delayed "
          "answer; intermediate Responses part stops do not: PASS")


def check_reasoning_alias(binary, up):
    with tempfile.TemporaryDirectory() as directory:
        repo = os.path.join(directory, "transcripts.git")
        with Askai(binary, "-u", up.url(), "--transcripts", repo, "--participant", "aliases") as ai:
            expected = []
            for plain in (False, True):
                up.plain = plain
                try:
                    for fields, thought in (({"reasoning": "alias"}, "alias"),
                                            ({"reasoning": "duplicate", "reasoning_content": "primary"},
                                             "primary")):
                        if plain:
                            payload = {"choices": [{"message": {"role": "assistant", "content": "A", **fields},
                                                   "finish_reason": "stop"}]}
                        else:
                            payload = Stream(wire=sse_wire([
                                (None, {"choices": [{"index": 0, "delta": {**fields, "content": "A"}}]}),
                                (None, "[DONE]")]))
                        up.reply = lambda request, p=payload: (200, p)
                        assert text_of(ai.ask()) == "A"
                        expected.append(thought.encode())
                        wait_for(lambda: sorted(reasoning_tree(repo, "aliases").values()) == sorted(expected))
                finally:
                    up.plain = False
    print("reasoning archive: chat reasoning alias works without duplicate aliases: PASS")


def check_journal_growth(binary, up):
    # Many independently decoded pieces must make one blob, not one snapshot
    # and tree per piece. The answer is deliberately much larger than thoughts.
    pieces = [f"{i:08x} ää\n" for i in range(2000)]
    events = [(None, {"choices": [{"index": 0, "delta": {"reasoning_content": text}}]})
              for text in pieces]
    events += [(None, {"choices": [{"index": 0, "delta": {"content": "A" * 200_000}}]}),
               (None, "[DONE]")]
    up.reply = lambda request: (200, Stream(wire=sse_wire(events)))
    with tempfile.TemporaryDirectory() as directory:
        repo = os.path.join(directory, "transcripts.git")
        with Askai(binary, "-u", up.url(), "--transcripts", repo, "--participant", "growth") as ai:
            assert text_of(ai.ask()) == "A" * 200_000
        assert list(reasoning_tree(repo, "growth").values()) == ["".join(pieces).encode()]
        run = subprocess.run(["git", "--git-dir", repo, "rev-list", "--count",
                              "refs/heads/reasoning/growth"], check=True, capture_output=True, text=True)
        assert run.stdout.strip() == "1", run.stdout
        objects = subprocess.run(["git", "--git-dir", repo, "cat-file", "--batch-all-objects",
                                  "--batch-check=%(objecttype) %(objectsize)"],
                                 check=True, capture_output=True, text=True).stdout.splitlines()
        assert len(objects) <= 8, objects
        assert sum(int(line.split()[1]) for line in objects) < 2 * len("".join(pieces).encode()), objects
        assert not Path(repo, "askai-journal", "growth", "records.wal").exists()
        data_bytes = len("".join(pieces).encode())
        object_bytes = sum(int(line.split()[1]) for line in objects)
        disk_bytes = sum(os.stat(os.path.join(root, name)).st_size
                         for root, _, names in os.walk(os.path.join(repo, "objects")) for name in names)
    print(f"reasoning archive: 2000 deltas, {data_bytes} plaintext bytes, 1 commit, "
          f"{len(objects)} Git objects/{object_bytes} logical bytes/{disk_bytes} stored bytes: PASS")


def check_journal_crash(binary, up):
    thought = "survives SIGKILL as decoded plaintext ää\n"
    gate = Gate(sse_wire([(None, {"choices": [{"index": 0,
                 "delta": {"reasoning_content": thought}}]})]), sse_wire([(None, "[DONE]")]))
    up.reply = lambda request: (200, gate)
    with tempfile.TemporaryDirectory() as directory:
        repo = os.path.join(directory, "transcripts.git")
        args = ("-u", up.url(), "--transcripts", repo, "--participant", "partial")
        ai = Askai(binary, *args)
        conn = None
        try:
            conn, response = begin(ai)
            assert event(response)["params"]["kind"] == "reasoning"
            wait_for(lambda: len(reasoning_files(repo, "partial")) == 1)
            path = reasoning_files(repo, "partial")[0]
            wait_for(lambda: path.read_bytes() == thought.encode())
            assert not reasoning_tree(repo, "partial")
            ai.proc.kill()
            assert ai.proc.wait(timeout=5) == -signal.SIGKILL
            ai.state.cleanup()
            assert path.read_bytes() == thought.encode()
            with Askai(binary, *args):
                assert path.read_bytes() == thought.encode()
                assert not reasoning_tree(repo, "partial")
        finally:
            gate.release.set()
            if conn:
                conn.close()
            if ai.proc.poll() is None:
                ai.proc.kill()
                ai.proc.wait(timeout=5)
    print("reasoning archive: unfinished plaintext survives SIGKILL and restart: PASS")


def check_cancel(binary, up):
    text = "cancel me " * 600_000
    # One client loop reuses the cancelled job immediately. Repetition covers
    # cancellation racing an already-scheduled provider feed step: that step
    # must not republish a mailbox after the front has relinquished it.
    with tempfile.TemporaryDirectory() as directory, \
            Askai(binary, "-u", up.url(), "--transcripts", directory + "/transcripts.git", loops=1) as ai:
        for _ in range(12):
            up.reply = lambda request: (200, turn_stream(text=text, reasoning="cancelled thought"))
            conn, response = begin(ai)
            assert event(response)["method"] == "notifications/askai/delta"
            response.close()
            conn.close()
            wait_for(lambda: ai.metrics()["askai_jobs_in_flight"] == 0)
            up.reply = lambda request: (200, ANSWERS["openai"])
            assert text_of(ai.ask()) == "A"
    print("chat stream: disconnect releases a backpressured mailbox and upstream slot: PASS")


def check_backpressure_timeout(binary, up):
    text = "slow reader " * 500_000
    # The comments keep reception active beyond the first large mailbox.
    # Its consumer must drain that mailbox before curl can receive the end.
    wire = sse_wire([(None, {"choices": [{"delta": {"content": text}}]})]) + \
        b": waiting\n\n" * 200_000
    with Askai(binary, "-u", up.url(), "--chunk-timeout", "1", "-T", "20",
               "--send-timeout", "10", "--output-pause", "16K") as ai:
        for completes in (True, False):
            payload = Stream(wire=wire + (sse_wire([(None, "[DONE]")]) if completes else b""),
                             stall=0 if completes else 3)
            up.reply = lambda request, p=payload: (200, p)
            conn, response = begin(ai, receive_buffer=16_384)
            first = event(response)
            assert first["method"] == "notifications/askai/delta", first
            time.sleep(1.5)
            values = ai.metrics()
            assert values["askai_upstream_stalls_total"] == 0, values
            assert values["askai_jobs_in_flight"] == 1, values
            messages = finish(response, [first])
            conn.close()
            assert "".join(d["text"] for d in deltas(messages)) == text
            result = messages[-1]["result"]
            if completes:
                assert text_of(result) == text
            else:
                assert result["isError"] and text_of(result) == \
                    "Upstream answer stalled: nothing came for 1 s.", result
                assert ai.metrics()["askai_upstream_stalls_total"] == 1
    print("chat stream: mailbox pauses suspend upstream silence; resumed stalls time out: PASS")


def main():
    binary = os.path.abspath(sys.argv[1])
    up = Upstream()
    try:
        check_apis(binary, up)
        check_large(binary, up)
        check_tools(binary, up)
        check_journal(binary, up)
        check_journal_stops(binary, up)
        check_reasoning_alias(binary, up)
        check_journal_growth(binary, up)
        check_journal_crash(binary, up)
        check_cancel(binary, up)
        check_backpressure_timeout(binary, up)
    finally:
        up.close()


if __name__ == "__main__":
    main()
