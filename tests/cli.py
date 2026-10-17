#!/usr/bin/env python3
"""Unix-socket chat checks against AskAI and a small malformed-peer fixture."""

import errno
import fcntl
import http.server
import json
import os
import pty
import select
import signal
import socket
import socketserver
import struct
import subprocess
import sys
import tempfile
import termios
import threading
import time

from askai import Askai, Stream, Upstream, sse_wire, turn_stream


def command(binary, path, *args):
    return [binary, "--socket", path, "--simple-io", *args]


def run(binary, path, *args, text=None):
    return subprocess.run(command(binary, path, *args), input=text,
                          capture_output=True, text=True, timeout=15)


def succeeded(result):
    assert result.returncode == 0, (result.returncode, result.stdout, result.stderr)
    return result.stdout


def answer(text):
    return {"choices": [{"message": {"role": "assistant", "content": text,
                                    "reasoning_content": "private reasoning"},
                         "finish_reason": "stop"}]}


def text_of(message):
    content = message.get("content", "")
    return content if isinstance(content, str) else "".join(
        part.get("text", "") for part in content)


def history(request):
    return [(message["role"], text_of(message))
            for message in request["body"]["messages"]
            if message["role"] in ("user", "assistant")]


class Session:
    """A real AskAI server and its Unix listener, with a fake upstream."""

    def __init__(self, binary, upstream, *args):
        self.directory = tempfile.TemporaryDirectory(prefix="askai-cli-")
        self.socket = f"{self.directory.name}/cli.sock"
        self.server = Askai(binary, "-u", upstream.url(), "-l", "unix:" + self.socket,
                            *args)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        try:
            self.server.__exit__(*exc)
        finally:
            self.directory.cleanup()


def check_options(cli, unused_server):
    help_result = subprocess.run([cli, "--help"], capture_output=True, text=True,
                                 timeout=5)
    assert help_result.returncode == 0, help_result
    assert "--socket" in help_result.stdout, help_result.stdout
    for option in ("--model", "--gpu-layers", "--ctx-size"):
        result = subprocess.run([cli, option, "1"], capture_output=True, text=True,
                                timeout=5)
        assert result.returncode != 0, (option, result)
    with tempfile.TemporaryDirectory() as directory:
        result = run(cli, directory + "/missing", "--single-turn", "--prompt", "Q")
        assert result.returncode != 0 and result.stderr, result
    # The client must not link the inference runtime, even indirectly.
    if sys.platform.startswith("linux"):
        dependencies = subprocess.run(["ldd", cli], capture_output=True, text=True,
                                      timeout=5)
        assert "libllama" not in dependencies.stdout.lower(), dependencies.stdout
        assert "libggml" not in dependencies.stdout.lower(), dependencies.stdout
    print("cli options: explicit socket, rejected inference flags, no model runtime: PASS")


def check_history(cli, server):
    upstream = Upstream()
    try:
        upstream.reply = lambda request: (200, answer(
            "first-answer" if len(upstream.requests) == 1 else "second-answer"))
        # Foreground chat must also work when the server defaults to background.
        with Session(server, upstream, "--background") as session:
            output = succeeded(run(cli, session.socket, "--no-reasoning",
                                   text="first question\nsecond question\n/exit\n"))
            assert output.count("first-answer") == output.count("second-answer") == 1, output
            assert "private reasoning" not in output, output
            assert len(upstream.requests) == 2, upstream.requests
            assert history(upstream.requests[0]) == [("user", "first question\n")], \
                upstream.requests[0]
            assert history(upstream.requests[1]) == [
                ("user", "first question\n"), ("assistant", "first-answer"),
                ("user", "second question\n")], upstream.requests[1]
            assert not upstream.requests[0]["body"].get("background", False), upstream.requests[0]
    finally:
        upstream.close()
    print("cli history: two native turns and foreground override: PASS")


def check_input(cli, server):
    upstream = Upstream()
    try:
        upstream.reply = lambda request: (200, answer("received"))
        with Session(server, upstream) as session:
            prompt = "first line\nsecond line with ää and λ\nlast line"
            output = succeeded(run(cli, session.socket, "--single-turn", "--prompt", prompt))
            assert output.count("received") == 1, output
            assert history(upstream.last()) == [("user", prompt)], upstream.last()
            with open(f"{session.directory.name}/prompt.txt", "w") as file:
                file.write(prompt)
            output = succeeded(run(cli, session.socket,
                                   text=f"/read {file.name}\n/exit\n"))
            assert output.count("received") == 1, output
            assert history(upstream.last()) == [("user", prompt)], upstream.last()
            for args, entered in (((), "one\\\ntwo\n"),
                                  (("--multiline-input",), "one\ntwo/\n")):
                output = succeeded(run(cli, session.socket, "--single-turn", *args, text=entered))
                assert output.count("received") == 1, output
                assert history(upstream.last()) == [("user", "one\ntwo\n")], upstream.last()
    finally:
        upstream.close()
    print("cli input: multiline UTF-8, /read, continuation and multiline submission: PASS")


def check_commands(cli, server):
    upstream = Upstream()
    try:
        upstream.reply = lambda request: (200, answer(f"answer-{len(upstream.requests)}"))
        with Session(server, upstream) as session:
            output = succeeded(run(cli, session.socket,
                                   text="one\ntwo\n/regen\n/clear\nthree\n/exit\n"))
            assert len(upstream.requests) == 4, (output, upstream.requests)
            assert history(upstream.requests[2]) == [
                ("user", "one\n"), ("assistant", "answer-1"), ("user", "two\n")], \
                upstream.requests[2]
            assert history(upstream.requests[3]) == [("user", "three\n")], upstream.requests[3]
    finally:
        upstream.close()
    print("cli commands: regenerate the last turn and clear local history: PASS")


def read_until(proc, needle, timeout, descriptor=None):
    """Read stdout through needle without waiting for the HTTP response to end."""
    if descriptor is None:
        descriptor = proc.stdout.fileno()
    output = b""
    until = time.monotonic() + timeout
    while needle not in output and time.monotonic() < until:
        ready, _, _ = select.select([descriptor], [], [], max(0, until - time.monotonic()))
        if ready:
            chunk = os.read(descriptor, 65536)
            if not chunk:
                break
            output += chunk
    assert needle in output, (output, proc.poll())
    return output


def check_stream(cli, server):
    upstream = Upstream()
    prefix = sse_wire([(None, {"choices": [{"index": 0, "delta": {
        "role": "assistant", "content": "streamed-prefix "}}]})])
    suffix = sse_wire([
        (None, {"choices": [{"index": 0, "delta": {"content": "streamed-suffix"}}]}),
        (None, {"choices": [{"index": 0, "delta": {}, "finish_reason": "stop"}]}),
        (None, "[DONE]")])
    upstream.reply = lambda request: (200, Stream(wire=prefix + suffix,
                                                sizes=(len(prefix),), gap=2.0))
    try:
        with Session(server, upstream) as session:
            proc = subprocess.Popen(command(cli, session.socket, "--single-turn", "--prompt", "Q"),
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            try:
                assert upstream.arrived.wait(5), "client did not submit a prompt"
                first = read_until(proc, b"streamed-prefix", 1.5)
                assert proc.poll() is None, "answer was buffered until completion"
                rest, errors = proc.communicate(timeout=10)
                assert proc.returncode == 0, (proc.returncode, first + rest, errors)
                output = first + rest
                assert output.count(b"streamed-prefix") == output.count(b"streamed-suffix") == 1, output
            finally:
                if proc.poll() is None:
                    proc.kill()
                proc.wait()
    finally:
        upstream.close()
    print("cli streaming: deltas displayed before completion, final answer not duplicated: PASS")


def check_tools(cli, server):
    upstream = Upstream()
    try:
        with tempfile.TemporaryDirectory(prefix="askai-cli-git-") as directory:
            repo = directory + "/proj"
            subprocess.run(["git", "init", "-q", repo], check=True)
            subprocess.run(["git", "-C", repo, "-c", "user.name=test", "-c",
                            "user.email=t@t", "commit", "--allow-empty", "-q", "-m",
                            "cli server tool result"], check=True)
            arguments = json.dumps({"repo": "proj", "command": "log", "options": ["--oneline"]})

            def reply(request):
                if len(upstream.requests) == 1:
                    return 200, turn_stream(calls=[("git-call", "git", arguments)])
                assert any(message.get("role") == "tool" and
                           "cli server tool result" in text_of(message)
                           for message in request["body"]["messages"]), request
                return 200, turn_stream(text="used server-side git")

            upstream.reply = reply
            with Session(server, upstream, "--upstream-tools", "--git", repo) as session:
                output = succeeded(run(cli, session.socket, "--single-turn", "--prompt", "Read the log"))
                assert output.count("used server-side git") == 1, output
                assert len(upstream.requests) == 2, upstream.requests
    finally:
        upstream.close()
    print("cli tools: AskAI executes the upstream tool call and continues the answer: PASS")


def check_cancel(cli, server):
    upstream = Upstream()
    upstream.reply = lambda request: (200, answer("too late"), 20)
    try:
        with Session(server, upstream) as session:
            for signum in (signal.SIGINT, signal.SIGTERM):
                upstream.arrived.clear()
                upstream.cancelled.clear()
                master, slave = pty.openpty()
                before = termios.tcgetattr(slave)
                proc = subprocess.Popen([cli, "--socket", session.socket, "--single-turn", "--no-history",
                                         "--prompt", "cancel me"], stdin=slave,
                                        stdout=slave, stderr=slave)
                try:
                    assert upstream.arrived.wait(5), "client did not submit a prompt"
                    assert termios.tcgetattr(slave) == before, \
                        "Readline changed terminal settings while no prompt was active"
                    proc.send_signal(signum)
                    status = proc.wait(timeout=5)
                    assert status == 128 + signum, (signum, status)
                    assert upstream.cancelled.wait(5), "signal left the upstream request running"
                    assert termios.tcgetattr(slave) == before, "terminal settings were not restored"
                finally:
                    if proc.poll() is None:
                        proc.kill()
                    proc.wait()
                    os.close(master)
                    os.close(slave)
    finally:
        upstream.close()
    print("cli cancellation: Ctrl-C/SIGTERM cancel the request and restore the terminal: PASS")


def check_broken_pipe(cli, server):
    upstream = Upstream()
    try:
        with Session(server, upstream) as session:
            reader, writer = os.pipe()
            os.close(reader)
            try:
                proc = subprocess.Popen(command(cli, session.socket, "--single-turn", "--prompt", "Q"),
                                        stdin=subprocess.DEVNULL, stdout=writer, stderr=subprocess.PIPE)
            finally:
                os.close(writer)
            try:
                _, errors = proc.communicate(timeout=10)
                assert proc.returncode != 0, proc.returncode
                assert b"write" in errors.lower(), errors
            finally:
                if proc.poll() is None:
                    proc.kill()
                proc.wait()
    finally:
        upstream.close()
    print("cli output: a closed stdout pipe is reported as failure: PASS")


class UnixServer(socketserver.ThreadingMixIn, socketserver.UnixStreamServer):
    daemon_threads = True

    def handle_error(self, request, address):
        if not isinstance(sys.exc_info()[1], (ConnectionError, OSError)):
            super().handle_error(request, address)


class Peer:
    """An HTTP MCP peer for errors and SSE fragmentation, bound only to Unix."""

    def __init__(self, reply, method_reply=None):
        self.directory = tempfile.TemporaryDirectory(prefix="askai-cli-peer-")
        self.socket = self.directory.name + "/mcp.sock"
        self.requests = []
        peer = self

        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *args):
                pass

            def do_POST(self):
                request = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
                peer.requests.append(request)
                method = request.get("method")
                status, kind, body = 200, "application/json", None
                custom = method_reply(request) if method_reply else None
                if custom is not None:
                    status, kind, body = custom
                elif method == "initialize":
                    body = {"result": {"protocolVersion": "2025-11-25", "capabilities": {"tools": {}},
                                       "serverInfo": {"name": "test", "version": "1"}}}
                elif method == "tools/list":
                    body = {"result": {"tools": [{"name": "ask_ai", "title": "Ask AI", "inputSchema": {
                        "type": "object", "properties": {"prompt": {"type": "string"},
                        "messages": {"type": "array"}, "background": {"type": "boolean"}}}}]}}
                elif method == "tools/call":
                    status, kind, body = reply(request)
                elif "id" not in request:
                    status, body = 202, b""
                else:
                    body = {"result": {}}
                if isinstance(body, dict):
                    body |= {"jsonrpc": "2.0", "id": request.get("id")}
                    body = json.dumps(body).encode()
                self.send_response(status)
                self.send_header("Content-Type", kind)
                self.send_header("Mcp-Session-Id", "cli-test-session")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                if kind == "text/event-stream":
                    # Deliberately split event names, JSON strings, and newlines.
                    for offset in range(0, len(body), 7):
                        self.wfile.write(body[offset:offset + 7])
                        self.wfile.flush()
                else:
                    self.wfile.write(body)

        self.server = UnixServer(self.socket, Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True)
        self.thread.start()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.server.shutdown()
        self.server.server_close()
        self.thread.join()
        self.directory.cleanup()


def check_peer_autocomplete_error(cli, unused_server):
    def reply(request):
        raise AssertionError(f"autocomplete made a tool call: {request}")

    with Peer(reply) as peer:
        master, slave = pty.openpty()
        before = termios.tcgetattr(slave)
        environment = os.environ | {"TERM": "xterm-256color", "LC_ALL": "C.UTF-8",
                                    "INPUTRC": os.devnull}
        proc = subprocess.Popen([cli, "--socket", peer.socket, "--color", "off", "--no-history"],
                                stdin=slave, stdout=slave, stderr=slave, env=environment)
        try:
            read_until(proc, b"> ", 5, master)
            # Exercise cli_run's callback, not the console fixture's callback:
            # malformed pasted UTF-8 makes autocomplete_complete return EINVAL.
            # Tab must leave editing alive so the user can repair the input.
            os.write(master, b"\x1b[200~\xff\x1b[201~\t\x15/he\t\r")
            read_until(proc, b"PgUp/PgDn search history by prefix", 5, master)
            os.write(master, b"/ex\t\r")
            assert proc.wait(timeout=5) == 0, proc.returncode
            assert termios.tcgetattr(slave) == before, "terminal settings were not restored"
            assert not any(request["method"] == "tools/call" for request in peer.requests), peer.requests
        finally:
            if proc.poll() is None:
                proc.kill()
            proc.wait()
            os.close(master)
            os.close(slave)
    print("cli autocomplete: rejected input preserves editing and subsequent Tab completion: PASS")


def check_peer_errors(cli, unused_server):
    cases = [
        (200, "application/json", b'{"broken":', None),
        (200, "text/event-stream", b'data: {"jsonrpc":"2.0","method":"notifications/ping"}\n\n', None),
        (200, "application/json", {"error": {"code": -32603, "message": "test RPC failure"}}, "test RPC failure"),
        (200, "application/json", {"result": {"isError": True, "content": [
            {"type": "text", "text": "test tool failure"}]}}, "test tool failure"),
        (200, "application/json", {"result": {"content": [
            {"type": "text\u0000suffix", "text": "must not be accepted"}]}}, "unsupported"),
        (401, "application/json", b'{"error":"denied"}', "401"),
        (200, "text/event-stream", sse_wire([(None, {
            "jsonrpc": "2.0", "method": "notifications/askai/delta",
            "params": {"requestId": -999, "kind": "text", "text": "WRONG"}})]),
         "request id"),
    ]
    for status, kind, body, message in cases:
        with Peer(lambda request: (status, kind, body)) as peer:
            result = run(cli, peer.socket, "--single-turn", "--prompt", "Q")
            assert result.returncode != 0, (status, kind, result)
            assert result.stderr, result
            if message:
                assert message in result.stderr, (message, result)
    print("cli errors: malformed JSON/SSE, wrong IDs, RPC/tool failures, HTTP denial: PASS")

    def wrong_version(request):
        if request.get("method") == "initialize":
            return 200, "application/json", {"result": {"protocolVersion": "2025-11-25\u0000suffix"}}

    with Peer(lambda request: (200, "application/json", {"result": {}}), wrong_version) as peer:
        result = run(cli, peer.socket, "--single-turn", "--prompt", "Q")
        assert result.returncode != 0 and "negotiate" in result.stderr, result
        assert len(peer.requests) == 1, peer.requests
    print("cli protocol strings: embedded NUL cannot hide a version or content-type suffix: PASS")


def check_peer_stream(cli, unused_server):
    def reply(request):
        request_id = request["id"]
        delta = lambda ident, kind, text: {"jsonrpc": "2.0", "method": "notifications/askai/delta",
            "params": {"requestId": ident, "kind": kind, "text": text}}
        events = [
            ("message", delta(request_id, "reasoning", "HIDDEN")),
            ("message", delta(request_id, "text", "héllo ")),
            ("message", delta(request_id, "text", "λ")),
            ("message", {"jsonrpc": "2.0", "id": request_id, "result": {"content": [
                {"type": "text", "text": "héllo λ"},
                {"type": "text", "text": "AUDIT TRACE"}]}}),
        ]
        return 200, "text/event-stream", sse_wire(events).replace(b"\n", b"\r\n")

    with Peer(reply) as peer:
        output = succeeded(run(cli, peer.socket, "--single-turn", "--no-reasoning", "--prompt", "Q"))
        assert output.strip() == "héllo λ", output
        call = next(request for request in peer.requests if request["method"] == "tools/call")
        assert call["params"]["arguments"]["background"] is False, call
    print("cli SSE: fragmented CRLF/UTF-8, hidden reasoning and trace: PASS")


def check_peer_progress(cli, unused_server):
    def reply(request):
        token = request["params"]["_meta"]["progressToken"]
        events = []

        def progress(text, kind="status"):
            params = {"progressToken": token, "progress": len(events), "message": text}
            if kind:
                params["_meta"] = {"askai": {"kind": kind}}
            events.append((None, {"jsonrpc": "2.0", "method": "notifications/progress",
                                  "params": params}))

        def delta(turn, kind, text):
            events.append((None, {"jsonrpc": "2.0", "method": "notifications/askai/delta",
                                  "params": {"requestId": request["id"], "turn": turn,
                                             "kind": kind, "text": text}}))

        progress("turn 1, generating, 0 tokens")
        for count, text in enumerate(("The", " user", " says hi."), 1):
            delta(1, "reasoning", text)
            progress(f"turn 1, thinking, {count} tokens")
        progress("turn 1, calling notes", "tool")
        # Untyped progress from older servers must also leave deltas intact.
        progress("turn 2, generating, 0 tokens", None)
        delta(2, "reasoning", "Read")
        progress("turn 2, thinking, 1 tokens", None)
        delta(2, "reasoning", " the note.")
        delta(2, "text", "Hello")
        progress("turn 2, generating, 3 tokens", None)
        delta(2, "text", " there.")
        progress("turn 2, generating, 4 tokens", None)
        events.append((None, {"jsonrpc": "2.0", "id": request["id"], "result": {
            "content": [{"type": "text", "text": "Hello there."}]}}))
        return 200, "text/event-stream", sse_wire(events)

    with Peer(reply) as peer:
        output = succeeded(run(cli, peer.socket, "--single-turn", "--prompt", "Q"))
        assert output == (
            "* ask_ai turn 1, generating, 0 tokens\n"
            "[Start thinking]\n\n"
            "The user says hi.\n[End thinking]\n"
            "* ask_ai turn 1, thinking, 3 tokens\n"
            "* ask_ai turn 1, calling notes\n"
            "* ask_ai turn 2, generating, 0 tokens\n"
            "[Start thinking]\n\n"
            "Read the note.\n[End thinking]\n\n"
            "Hello there.\n"
            "* ask_ai turn 2, generating, 4 tokens\n"), output
    print("cli progress: uninterrupted reasoning/text, coalesced counts and immediate tools: PASS")


def check_peer_catalog(cli, unused_server):
    generation = 0
    ask = {"name": "ask_ai", "title": "Ask AI", "inputSchema": {
        "type": "object", "properties": {"prompt": {"type": "string"},
        "messages": {"type": "array"}, "background": {"type": "boolean"}}}}
    git = {"name": "git", "description": "Read repository history", "inputSchema": {
        "type": "object", "additionalProperties": False, "properties": {
            "command": {"type": "string", "enum": ["log", "status"]},
            "count": {"type": "integer"}, "recursive": {"type": "boolean"},
            "message": {"type": "string"}}}}
    notes = {"name": "notebook_service", "_meta": {"askai": {
        "participant": "self", "notebooks": True}}, "inputSchema": {
            "type": "object", "properties": {"action": {"enum": ["notebooks"]}}}}
    lookup = {"name": "lookup", "inputSchema": {"type": "object", "properties": {
        "query": {"type": "string"}}}}

    def result(value):
        return 200, "application/json", {"result": value}

    def methods(request):
        nonlocal generation
        if request["method"] != "tools/list":
            return None
        cursor = request.get("params", {}).get("cursor")
        if cursor is None:
            generation += 1
            return result({"tools": [ask], "nextCursor": "remaining"})
        assert cursor == "remaining", request
        return result({"tools": [git, notes] + ([lookup] if generation > 1 else [])})

    def reply(request):
        params = request["params"]
        name = params["name"]
        if name == "notebook_service":
            assert params["arguments"] == {"action": "notebooks"}, request
            names = ("alice/anonymous/client\nbob/0123456789abcdef/upstream\n"
                     "alice/anonymous/upstream\nmalformed line\n"
                     "bad/invalid/client\ntruncated/anonymous/clien") if generation == 1 else \
                    "carol/anonymous/client\n"
            return result({"content": [{"type": "text", "text": names}]})
        if name == "git":
            assert params["arguments"] == {"command": "log", "count": 2,
                                            "recursive": True, "message": "hello world"}, request
            token = params["_meta"]["progressToken"]
            events = [
                (None, {"jsonrpc": "2.0", "method": "notifications/progress", "params": {
                    "progressToken": "other", "progress": 1, "message": "WRONG-PROGRESS"}}),
                (None, {"jsonrpc": "2.0", "method": "notifications/progress", "params": {
                    "progressToken": token, "progress": 1, "message": "reading history"}}),
                (None, {"jsonrpc": "2.0", "id": request["id"], "result": {
                    "content": [{"type": "text", "text": "TOOL-RESULT"}]}}),
            ]
            return 200, "text/event-stream", sse_wire(events)
        if name == "lookup":
            assert params["arguments"] == {"query": "latest"}, request
            return result({"content": [{"type": "text", "text": "REFRESHED-TOOL"}]})
        assert name == "ask_ai", request
        assert params["arguments"]["messages"] == [], request
        assert params["arguments"]["prompt"] == "Q\n", request
        return result({"content": [{"type": "text", "text": "CLI-RESULT"}]})

    entered = ("/tools git\n/names\n"
               "/tool git command=log count=2 recursive=true message='hello world'\n"
               "Q\n/refresh\n/names\n/tools\n/tool lookup query=latest\n/exit\n")
    with Peer(reply, methods) as peer:
        output = succeeded(run(cli, peer.socket, text=entered))
        for marker in ("TOOL-RESULT", "CLI-RESULT", "REFRESHED-TOOL", "alice", "bob", "carol", "self"):
            assert marker in output, (marker, output)
        assert "malformed" not in output and "truncated" not in output, output
        assert "* you call git" in output and "* git reading history" in output, output
        assert "* git completed" in output and "WRONG-PROGRESS" not in output, output
        calls = [request["params"]["name"] for request in peer.requests
                 if request["method"] == "tools/call"]
        assert calls == ["notebook_service", "git", "ask_ai", "notebook_service", "lookup"], calls
        assert generation == 2, (generation, peer.requests)
    print("cli catalog: paginated tools, typed invocation, notes participants and refresh: PASS")


def check_peer_catalog_changed(cli, unused_server):
    lists = 0

    def methods(request):
        nonlocal lists
        if request["method"] == "tools/list":
            lists += 1
        return None

    def reply(request):
        events = [
            (None, {"jsonrpc": "2.0", "method": "notifications/tools/list_changed"}),
            (None, {"jsonrpc": "2.0", "id": request["id"], "result": {
                "content": [{"type": "text", "text": "changed"}]}}),
        ]
        return 200, "text/event-stream", sse_wire(events)

    with Peer(reply, methods) as peer:
        output = succeeded(run(cli, peer.socket, text="Q\n/exit\n"))
        assert "changed" in output, output
        assert lists == 2, (lists, peer.requests)
    print("cli catalog: tools/list_changed refreshes before the next prompt: PASS")


def check_peer_notes(cli, unused_server):
    """Discover server note paths, then exercise the actual Readline callback."""
    own = "design/äiti (1 line).md"
    other = "thoughts/next.md"
    selected = "reviews/note  (1 line).md"
    selector = "bob/anonymous/upstream"
    copied, learned = False, False
    calls = []
    ask = {"name": "ask_ai", "title": "Ask AI", "inputSchema": {
        "type": "object", "properties": {"prompt": {"type": "string"},
        "messages": {"type": "array"}, "background": {"type": "boolean"}}}}
    notes = {"name": "notebook_service", "_meta": {"askai": {
        "participant": "self", "notebooks": True}}, "inputSchema": {
            "type": "object", "properties": {
                "action": {"type": "string", "enum": ["notebooks", "list", "read", "copy", "add"]},
                "notebook": {"type": "string"}, "file": {"type": "string"}}}}

    def result(text):
        return 200, "application/json", {"result": {"content": [
            {"type": "text", "text": text}]}}

    def methods(request):
        if request["method"] == "tools/list":
            return 200, "application/json", {"result": {"tools": [ask, notes]}}
        return None

    def reply(request):
        nonlocal copied, learned
        params = request["params"]
        if params["name"] == "ask_ai":
            learned = True
            return result("MODEL-NOTED")
        assert params["name"] == "notebook_service", request
        args = params["arguments"]
        if args["action"] == "notebooks":
            return result("Session notebooks (use a selector as notebook):\n" + selector + "\n")
        if args["action"] == "list":
            book = args["notebook"]
            assert book in ("own", "other", selector), request
            files = [own] if book == "own" else [other] if book == "other" else [selected]
            if book == "own":
                files += (["copied/new note.md"] if copied else [])
                files += (["learned/server note.md"] if learned else [])
            count = "2 lines" if book == "own" else "binary" if book == "other" else "1 line"
            records = "".join(f"{index}. 2026-10-17 01:23 UTC  {name}  ({count})\n"
                              for index, name in enumerate(files, 1))
            return result("Selected notebook, oldest first:\n\n" + records +
                          "77. 2026-10-17 01:23 UTC  partial.md  (1 line)")
        assert args["action"] in ("read", "copy"), request
        calls.append(args)
        copied |= args["action"] == "copy"
        return result("NOTE-OK")

    with Peer(reply, methods) as peer:
        master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 240, 0, 0))
        before = termios.tcgetattr(slave)
        environment = os.environ | {"TERM": "xterm-256color", "LC_ALL": "C.UTF-8",
                                    "INPUTRC": os.devnull,
                                    "XDG_STATE_HOME": peer.directory.name + "/state"}
        proc = subprocess.Popen([cli, "--socket", peer.socket, "--color", "off", "--no-history"],
                                stdin=slave, stdout=slave, stderr=slave, env=environment)
        try:
            read_until(proc, b"> ", 5, master)
            before_tab = len(peer.requests)
            os.write(master, b"/tool notebook_service action=read file=des\t")
            read_until(proc, own.encode(), 5, master)
            assert len(peer.requests) == before_tab, "Tab performed network discovery"
            os.write(master, b"\r")
            read_until(proc, b"\n> ", 5, master)
            assert calls[-1] == {"action": "read", "file": own}, calls

            prefix = "/tool notebook_service action=read file=re"
            line = prefix + " notebook='" + selector + "'"
            os.write(master, line.encode() + b"\x01" + b"\x1b[C" * len(prefix) + b"\t\r")
            read_until(proc, b"\n> ", 5, master)
            assert calls[-1] == {"action": "read", "file": selected, "notebook": selector}, calls

            os.write(master, b"/tool notebook_service action=copy file=th\t\r")
            read_until(proc, b"\n> ", 5, master)
            assert calls[-1] == {"action": "copy", "file": other}, calls

            os.write(master, b"/tool notebook_service action=read notebook=ow\tfile=cop\t\r")
            read_until(proc, b"\n> ", 5, master)
            assert calls[-1] == {"action": "read", "notebook": "own", "file": "copied/new note.md"}, calls

            os.write(master, b"Write a note\r")
            read_until(proc, b"\n> ", 5, master)
            os.write(master, b"/tool notebook_service action=read file=learn\t\r")
            read_until(proc, b"\n> ", 5, master)
            assert calls[-1] == {"action": "read", "file": "learned/server note.md"}, calls

            # A cut list record is not a filename candidate.
            os.write(master, b"/tool notebook_service action=read file=part\t\r")
            read_until(proc, b"\n> ", 5, master)
            assert calls[-1] == {"action": "read", "file": "part"}, calls
            os.write(master, b"/exit\r")
            assert proc.wait(timeout=5) == 0, proc.returncode
            assert termios.tcgetattr(slave) == before, "terminal settings were not restored"
        finally:
            if proc.poll() is None:
                proc.kill()
            proc.wait()
            os.close(master)
            os.close(slave)
    print("cli notes: server paths, selectors, UTF-8 quotes, mutation refresh and offline Tab: PASS")


def check_peer_history(cli, unused_server):
    calls = []

    def reply(request):
        calls.append(request["params"]["arguments"])
        return 200, "application/json", {"result": {"content": [
            {"type": "text", "text": f"saved-answer-{len(calls)}"}]}}

    with Peer(reply) as peer:
        repository = peer.directory.name + "/history.git"
        options = ("--history-repo", repository, "--session-id", "first-session")
        succeeded(run(cli, peer.socket, *options, text="first prompt\n"))
        output = succeeded(run(cli, peer.socket, *options, text="/history\nsecond prompt\n"))
        assert "first prompt" in output and "saved-answer-1" in output, output
        assert calls[1]["messages"] == [
            {"role": "user", "content": "first prompt\n"},
            {"role": "assistant", "content": "saved-answer-1"}], calls
        succeeded(run(cli, peer.socket, *options, text="/regen\n"))
        assert calls[2]["messages"] == calls[1]["messages"], calls
        assert calls[2]["prompt"] == "second prompt\n", calls
        output = succeeded(run(cli, peer.socket, *options, text="/history\n/clear\n"))
        assert "saved-answer-3" in output and "saved-answer-2" not in output, output
        output = succeeded(run(cli, peer.socket, *options, text="/history\nfresh prompt\n"))
        assert "saved-answer-1" not in output and "saved-answer-3" not in output, output
        assert calls[3]["messages"] == [], calls
        succeeded(run(cli, peer.socket, "--history-repo", repository,
                      "--session-id", "other-session", text="independent\n"))
        assert calls[4]["messages"] == [], calls
        # Redirected input remains stateless unless persistence is requested.
        for _ in range(2):
            succeeded(run(cli, peer.socket, text="stateless\n"))
            assert calls[-1]["messages"] == [], calls
        stored = subprocess.run(["git", "--git-dir=" + repository, "log", "--all", "--oneline"],
                                check=True, capture_output=True, text=True, timeout=5)
        assert stored.stdout, "no conversation snapshots were committed"
    print("cli persistence: restart, native replay, regen, clear and session isolation: PASS")


def check_peer_input_history(cli, unused_server):
    calls = []

    def reply(request):
        calls.append(request["params"]["arguments"])
        return 200, "application/json", {"result": {"content": [
            {"type": "text", "text": "remembered-answer"}]}}

    with Peer(reply) as peer:
        options = ("--history-repo", peer.directory.name + "/history.git",
                   "--session-id", "input-session")
        succeeded(run(cli, peer.socket, *options, text="remembered prompt\n/help\n"))
        master, slave = pty.openpty()
        before = termios.tcgetattr(slave)
        environment = os.environ | {"TERM": "xterm-256color", "LC_ALL": "C.UTF-8",
                                    "INPUTRC": os.devnull}
        proc = subprocess.Popen([cli, "--socket", peer.socket, "--color", "off", *options],
                                stdin=slave, stdout=slave, stderr=slave, env=environment)
        try:
            read_until(proc, b"\n> ", 5, master)
            os.write(master, b"\x1b[A\r")
            output = read_until(proc, b"\n> ", 5, master)
            assert b"PgUp/PgDn search history by prefix" in output, output
            assert len(calls) == 1, calls
            os.write(master, b"remembered\x1b[5~\r")
            output = read_until(proc, b"\n> ", 5, master)
            assert b"remembered-answer" in output, output
            assert len(calls) == 2 and calls[1]["prompt"] == "remembered prompt\n", calls
            assert calls[1]["messages"] == [
                {"role": "user", "content": "remembered prompt\n"},
                {"role": "assistant", "content": "remembered-answer"}], calls
            os.write(master, b"\x04")
            assert proc.wait(timeout=5) == 0, proc.returncode
            assert termios.tcgetattr(slave) == before, "terminal settings were not restored"
        finally:
            if proc.poll() is None:
                proc.kill()
            proc.wait()
            os.close(master)
            os.close(slave)
    print("cli input persistence: Up recalls commands and PgUp recalls prompts after restart: PASS")


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: cli.py ASKAI-CLI ASKAI")
    cli, server = map(os.path.abspath, sys.argv[1:])
    check_options(cli, server)
    try:
        probe = socket.socket(socket.AF_UNIX)
    except OSError as error:
        if error.errno in (errno.EPERM, errno.EAFNOSUPPORT, errno.EPROTONOSUPPORT):
            print(f"cli Unix integration: SKIP (socket(AF_UNIX): {error})")
            return
        raise
    else:
        probe.close()
    for check in (check_history, check_input, check_commands, check_stream,
                  check_tools, check_cancel, check_broken_pipe, check_peer_errors, check_peer_stream,
                  check_peer_progress,
                  check_peer_catalog, check_peer_catalog_changed, check_peer_autocomplete_error,
                  check_peer_notes, check_peer_history, check_peer_input_history):
        check(cli, server)


if __name__ == "__main__":
    main()
