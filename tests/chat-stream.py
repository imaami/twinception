#!/usr/bin/env python3
"""Opt-in live AskAI deltas, immutable mailbox flow control and persistence."""

import json
import os
import subprocess
import sys
import tempfile
import threading

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


def begin(ai, rpc_id=41):
    conn = ai.connect()
    conn.request("POST", "/mcp", json.dumps({
        "jsonrpc": "2.0", "id": rpc_id, "method": "tools/call", "params": {
            "name": "ask_ai", "arguments": {"prompt": "Q", "background": False},
            "_meta": {"askai": {"stream": True}}}}), headers={
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
        with Askai(binary, "-u", up.url(), "--git", repo, "--upstream-tools") as ai:
            conn, response = begin(ai)
            messages = finish(response)
            conn.close()
            assert "".join(d["text"] for d in deltas(messages) if d["turn"] == 1) == "Checking."
            assert "".join(d["text"] for d in deltas(messages) if d["turn"] == 2) == "Done."
            assert messages[-1]["result"]["content"][0]["text"] == "Done."
            assert "first" in up.last()["raw"].decode() and not replies
    print("chat stream: tool execution continues through distinct streamed turns: PASS")


def check_journal(binary, up):
    prefix = sse_wire([(None, {"choices": [{"index": 0, "delta": {
        "reasoning_content": "durable live reasoning"}}]}), (None, {"choices": [{"index": 0,
        "delta": {"content": "durable live answer"}}]})])
    gate = Gate(prefix, sse_wire([(None, "[DONE]")]))
    up.reply = lambda request: (200, gate)
    with tempfile.TemporaryDirectory() as directory:
        repo = os.path.join(directory, "transcripts.git")
        try:
            with Askai(binary, "-u", up.url(), "--transcripts", repo,
                       "--participant", "chat-test") as ai:
                conn, response = begin(ai)
                seen = []
                while not deltas(seen):
                    seen.append(event(response))
                # Both the raw event and exact outgoing notice are already committed
                # while the provider is still waiting to send its completion.
                run = subprocess.run(["git", "--git-dir", repo, "grep", "-F", "-q",
                                      "notifications/askai/delta", "refs/heads/transcripts/chat-test",
                                      "--", "records"], check=False)
                assert run.returncode == 0
                gate.release.set()
                messages = finish(response, seen)
                conn.close()
                assert text_of(messages[-1]["result"]) == "durable live answer"
        finally:
            gate.release.set()
        subprocess.run(["git", "--git-dir", repo, "fsck", "--full", "--strict"],
                       check=True, capture_output=True)
    print("chat stream: exact outgoing deltas committed before delivery: PASS")


def check_cancel(binary, up):
    text = "cancel me " * 600_000
    # One client loop reuses the cancelled job immediately. Repetition covers
    # cancellation racing an already-scheduled provider feed step: that step
    # must not republish a mailbox after the front has relinquished it.
    with Askai(binary, "-u", up.url(), loops=1) as ai:
        for _ in range(12):
            up.reply = lambda request: (200, turn_stream(text=text))
            conn, response = begin(ai)
            assert event(response)["method"] == "notifications/askai/delta"
            response.close()
            conn.close()
            wait_for(lambda: ai.metrics()["askai_jobs_in_flight"] == 0)
            up.reply = lambda request: (200, ANSWERS["openai"])
            assert text_of(ai.ask()) == "A"
    print("chat stream: disconnect releases a backpressured mailbox and upstream slot: PASS")


def main():
    binary = os.path.abspath(sys.argv[1])
    up = Upstream()
    try:
        check_apis(binary, up)
        check_large(binary, up)
        check_tools(binary, up)
        check_journal(binary, up)
        check_cancel(binary, up)
    finally:
        up.close()


if __name__ == "__main__":
    main()
