#!/usr/bin/env python3
"""End-to-end checks of crossed history and chat-template validation."""

import json
import subprocess
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class State:
    def __init__(self, name, drop_reasoning=False, prune_old=False):
        self.name = name
        self.drop_reasoning = drop_reasoning
        self.prune_old = prune_old
        self.requests = []
        self.probes = []
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

        def do_POST(self):
            length = int(self.headers.get("Content-Length", "0"))
            request = json.loads(self.rfile.read(length))

            if self.path == "/apply-template":
                with state.lock:
                    state.probes.append(request)
                prompt = []
                assistants = [i for i, message in enumerate(request["messages"])
                              if message.get("role") == "assistant"]
                latest_assistant = assistants[-1] if assistants else -1
                for i, message in enumerate(request["messages"]):
                    prompt.append(message.get("content", ""))
                    if (not state.drop_reasoning and
                            (not state.prune_old or i == latest_assistant)):
                        prompt.append(message.get("reasoning_content", ""))
                self.send_json({"prompt": "\n".join(prompt)})
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


def check_crossed_history(binary):
    states = (State("A"), State("B"))
    servers = servers_for(states)
    try:
        urls = urls_for(servers)
        run = subprocess.run(
            [binary, "-a", urls[0], "-b", urls[1]],
            input="first\nsecond\n",
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
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
    finally:
        stop_servers(servers)

    print("crossed-reasoning history: PASS")
    return 0


def check_rejected_template(binary):
    states = (State("A"), State("B", drop_reasoning=True))
    servers = servers_for(states)
    try:
        urls = urls_for(servers)
        run = subprocess.run(
            [binary, "-a", urls[0], "-b", urls[1], "-p", "must not run"],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        assert run.returncode != 0
        assert "model B template check failed" in run.stderr
        assert not states[0].requests
        assert not states[1].requests
    finally:
        stop_servers(servers)

    print("reasoning-dropping template rejection: PASS")
    return 0


def check_pruned_old_reasoning(binary):
    states = (State("A", prune_old=True), State("B"))
    servers = servers_for(states)
    try:
        urls = urls_for(servers)
        run = subprocess.run(
            [binary, "-a", urls[0], "-b", urls[1], "-p", "still runs"],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        assert run.returncode == 0
        assert "model A template check: older reasoning is pruned" in run.stderr
        assert len(states[0].requests) == len(states[1].requests) == 1
    finally:
        stop_servers(servers)

    print("latest-only reasoning template: PASS")
    return 0


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else "./twinception"
    return (check_crossed_history(binary) or
            check_pruned_old_reasoning(binary) or
            check_rejected_template(binary))


if __name__ == "__main__":
    raise SystemExit(main())
