#!/usr/bin/env python3
"""End-to-end check of crossed reasoning history."""

import json
import subprocess
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class State:
    def __init__(self, name):
        self.name = name
        self.requests = []
        self.lock = threading.Lock()


def handler_for(state):
    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, fmt, *args):
            pass

        def do_POST(self):
            length = int(self.headers.get("Content-Length", "0"))
            request = json.loads(self.rfile.read(length))
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
            self.wfile.write(wire)

    return Handler


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else "./twinception"
    states = (State("A"), State("B"))
    servers = [ThreadingHTTPServer(("127.0.0.1", 0), handler_for(state))
               for state in states]
    threads = [threading.Thread(target=server.serve_forever, daemon=True)
               for server in servers]
    for thread in threads:
        thread.start()

    try:
        urls = [f"http://127.0.0.1:{server.server_port}/v1/chat/completions"
                for server in servers]
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
        for server in servers:
            server.shutdown()
            server.server_close()

    print("crossed-reasoning history: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
