#!/usr/bin/env python3
"""End-to-end checks for crossed history, rapid swapping, and crosstalk."""

import json
import subprocess
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

RAPID_R = "__TWINCEPTION_RAPID_REASONING_8E6A09B3__"
RAPID_A = "__TWINCEPTION_RAPID_ANSWER_71C4F052__"
RAPID_TRANSITION = "<FINAL>"


class State:
    def __init__(self, name, drop_reasoning=False, prune_old=False):
        self.name = name
        self.drop_reasoning = drop_reasoning
        self.prune_old = prune_old
        self.requests = []
        self.probes = []
        self.completions = []
        self.quantum_count = 0
        self.final_count = 0
        self.lock = threading.Lock()


def handler_for(state):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, fmt, *args):
            pass

        def send_json(self, obj):
            wire = json.dumps(obj, separators=(",", ":")).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(wire)))
            self.end_headers()
            try:
                self.wfile.write(wire)
            except BrokenPipeError:
                pass

        def send_sse(self, objects):
            body = "".join(
                "data: " + json.dumps(obj, separators=(",", ":")) + "\n\n"
                for obj in objects
            )
            wire = body.encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Content-Length", str(len(wire)))
            self.end_headers()
            try:
                self.wfile.write(wire)
            except BrokenPipeError:
                pass

        def do_POST(self):
            length = int(self.headers.get("Content-Length", "0"))
            request = json.loads(self.rfile.read(length))

            if self.path == "/apply-template":
                with state.lock:
                    state.probes.append(request)

                messages = request["messages"]
                if any(m.get("reasoning_content") == RAPID_R for m in messages):
                    self.send_json({"prompt": "HEAD" + RAPID_R +
                                              RAPID_TRANSITION + RAPID_A})
                    return

                # A non-probe /apply-template request is a rapid turn render.
                # Normal inference uses /v1/chat/completions directly.
                if messages[-1].get("content") != "template probe 2":
                    user = messages[-1]["content"]
                    self.send_json({"prompt": f"BASE-{state.name}:{user}<THINK>"})
                    return

                prompt = []
                assistants = [i for i, message in enumerate(messages)
                              if message.get("role") == "assistant"]
                latest_assistant = assistants[-1] if assistants else -1
                for i, message in enumerate(messages):
                    prompt.append(message.get("content", ""))
                    if (not state.drop_reasoning and
                            (not state.prune_old or i == latest_assistant)):
                        prompt.append(message.get("reasoning_content", ""))
                self.send_json({"prompt": "\n".join(prompt)})
                return

            if self.path == "/completion":
                with state.lock:
                    state.completions.append(request)
                    if request.get("n_predict") == 2:
                        state.quantum_count += 1
                        number = state.quantum_count
                        final = False
                    else:
                        state.final_count += 1
                        number = state.final_count
                        final = True

                if not final:
                    text = f"{state.name.lower()}{number} "
                    event = {"content": text, "tokens": [number, number + 10],
                             "stop": True, "stop_type": "limit"}
                else:
                    event = {"content": f"F{state.name}{number}", "tokens": [99],
                             "stop": True, "stop_type": "limit"}
                self.send_sse([event])
                return

            if self.path != "/v1/chat/completions":
                self.send_error(404)
                return

            with state.lock:
                state.requests.append(request)
                turn = len(state.requests)

            chunks = (
                {"choices": [{"index": 0,
                              "delta": {"reasoning_content": f"R{state.name}{turn}"},
                              "finish_reason": None}]},
                {"choices": [{"index": 0,
                              "delta": {"content": f"A{state.name}{turn}"},
                              "finish_reason": None}]},
            )
            body = "".join(
                "data: " + json.dumps(chunk, separators=(",", ":")) + "\n\n"
                for chunk in chunks
            ) + "data: [DONE]\n\n"
            wire = body.encode()

            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Content-Length", str(len(wire)))
            self.end_headers()
            try:
                self.wfile.write(wire)
            except BrokenPipeError:
                pass

    return Handler


def servers_for(states):
    servers = [ThreadingHTTPServer(("127.0.0.1", 0), handler_for(state))
               for state in states]
    threads = [threading.Thread(target=server.serve_forever, daemon=True)
               for server in servers]
    for thread in threads:
        thread.start()
    return servers


def stop_servers(servers):
    for server in servers:
        server.shutdown()
        server.server_close()


def urls_for(servers):
    return [f"http://127.0.0.1:{server.server_port}/v1/chat/completions"
            for server in servers]


def run_client(binary, states, *args, input_text=None):
    servers = servers_for(states)
    try:
        urls = urls_for(servers)
        run = subprocess.run(
            [binary, "-a", urls[0], "-b", urls[1], *args],
            input=input_text,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
            timeout=10,
        )
        return run
    finally:
        stop_servers(servers)


def check_crossed_history(binary):
    states = (State("A"), State("B"))
    run = run_client(binary, states, input_text="first\nsecond\n")
    if run.returncode:
        sys.stderr.write(run.stdout)
        sys.stderr.write(run.stderr)
        return run.returncode

    a, b = (state.requests for state in states)
    assert len(a) == len(b) == 2
    assert all(len(state.probes) == 1 for state in states)
    assert a[1]["messages"][-2] == {
        "role": "assistant",
        "content": "AA1",
        "reasoning_content": "RB1",
    }
    assert b[1]["messages"][-2] == {
        "role": "assistant",
        "content": "AB1",
        "reasoning_content": "RA1",
    }
    assert a[0]["reasoning_format"] == "deepseek"
    assert b[0]["reasoning_format"] == "deepseek"

    print("crossed-reasoning history: PASS")
    return 0


def check_crosstalk(binary):
    states = (State("A"), State("B"))
    run = run_client(binary, states, "-C", "1", "-p", "seed")
    if run.returncode:
        sys.stderr.write(run.stdout)
        sys.stderr.write(run.stderr)
        return run.returncode

    a, b = (state.requests for state in states)
    assert len(a) == len(b) == 2
    assert a[1]["messages"][-1] == {"role": "user", "content": "AB1"}
    assert b[1]["messages"][-1] == {"role": "user", "content": "AA1"}
    assert a[1]["messages"][-2]["reasoning_content"] == "RB1"
    assert b[1]["messages"][-2]["reasoning_content"] == "RA1"

    print("autonomous crosstalk: PASS")
    return 0


def check_rapid_swap(binary):
    states = (State("A"), State("B"))
    run = run_client(binary, states, "-q", "2", "-R", "4", "-n", "8",
                     "-p", "seed")
    if run.returncode:
        sys.stderr.write(run.stdout)
        sys.stderr.write(run.stderr)
        return run.returncode

    a, b = (state.completions for state in states)
    assert len(a) == len(b) == 3
    assert a[0]["prompt"] == "BASE-A:seed<THINK>"
    assert b[0]["prompt"] == "BASE-B:seed<THINK>"
    assert a[1]["prompt"] == "BASE-A:seed<THINK>b1 "
    assert b[1]["prompt"] == "BASE-B:seed<THINK>a1 "
    assert a[2]["prompt"] == "BASE-A:seed<THINK>b1 b2 " + RAPID_TRANSITION
    assert b[2]["prompt"] == "BASE-B:seed<THINK>a1 a2 " + RAPID_TRANSITION
    assert all(req["cache_prompt"] is True for req in a + b)
    assert [req["n_predict"] for req in a] == [2, 2, 8]

    print("causal rapid thought swap: PASS")
    return 0


def check_rapid_crosstalk(binary):
    states = (State("A"), State("B"))
    run = run_client(binary, states, "-q", "2", "-R", "2", "-n", "8",
                     "-C", "1", "-p", "seed")
    if run.returncode:
        sys.stderr.write(run.stdout)
        sys.stderr.write(run.stderr)
        return run.returncode

    a, b = states
    assert len(a.completions) == len(b.completions) == 4
    turn_renders_a = [p for p in a.probes
                      if p["messages"][-1].get("content") != RAPID_A]
    turn_renders_b = [p for p in b.probes
                      if p["messages"][-1].get("content") != RAPID_A]
    # Startup rapid probe + two turn renders.
    assert len(a.probes) == len(b.probes) == 3
    assert turn_renders_a[-1]["messages"][-1] == {"role": "user", "content": "FB1"}
    assert turn_renders_b[-1]["messages"][-1] == {"role": "user", "content": "FA1"}
    assert a.completions[2]["prompt"].startswith("BASE-A:FB1<THINK>")
    assert b.completions[2]["prompt"].startswith("BASE-B:FA1<THINK>")

    print("rapid swap + crosstalk: PASS")
    return 0


def check_rejected_template(binary):
    states = (State("A"), State("B", drop_reasoning=True))
    run = run_client(binary, states, "-p", "must not run")
    assert run.returncode != 0
    assert "model B template check failed" in run.stderr
    assert not states[0].requests
    assert not states[1].requests

    print("reasoning-dropping template rejection: PASS")
    return 0


def check_pruned_old_reasoning(binary):
    states = (State("A", prune_old=True), State("B"))
    run = run_client(binary, states, "-p", "still runs")
    assert run.returncode == 0
    assert "model A template check: older reasoning is pruned" in run.stderr
    assert len(states[0].requests) == len(states[1].requests) == 1

    print("latest-only reasoning template: PASS")
    return 0


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else "./twinception"
    return (check_crossed_history(binary) or
            check_crosstalk(binary) or
            check_rapid_swap(binary) or
            check_rapid_crosstalk(binary) or
            check_pruned_old_reasoning(binary) or
            check_rejected_template(binary))


if __name__ == "__main__":
    raise SystemExit(main())
