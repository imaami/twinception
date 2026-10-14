#!/usr/bin/env python3
"""End-to-end checks for the askai MCP server against fake AI endpoints."""

import fcntl
import grp
import hashlib
import http.client
import json
import os
import pwd
import re
import resource
import select
import shutil
import signal
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

LEGACY = "2025-11-25"
MODERN = "2026-07-28"
# A modern request's _meta that takes tasks (the tasks extension).
TAKES_TASKS = {"io.modelcontextprotocol/protocolVersion": MODERN,
               "io.modelcontextprotocol/clientCapabilities": {
                   "extensions": {"io.modelcontextprotocol/tasks": {}}}}
KEY_VARS = ("ASKAI_TOKEN", "ASKAI_API_KEY", "LLAMA_API_KEY", "OPENAI_API_KEY",
            "ANTHROPIC_API_KEY", "GEMINI_API_KEY", "GOOGLE_API_KEY",
            "DEEPINFRA_API_KEY", "DEEPINFRA_TOKEN", "OPENROUTER_API_KEY")
# The tools that turns lent git are given, in order: git, then compact,
# then those that run git in the background, wait and set timers.
TURN_TOOLS = ["git", "compact", "background", "jobs", "wait", "timer"]
# The API that each answer format of the fake upstream is in.
API = {"openai": "chat", "responses": "responses", "anthropic": "messages",
       "gemini": "generate"}

ANSWERS = {
    "openai": {"choices": [{"message": {"role": "assistant", "content": "A",
                                        "reasoning_content": "hmm"},
                            "finish_reason": "stop"}]},
    "responses": {"status": "completed", "output": [
        {"type": "reasoning", "summary": []},
        {"type": "message", "content": [{"type": "output_text", "text": "A"}]}]},
    "anthropic": {"content": [{"type": "thinking", "thinking": "hmm"},
                              {"type": "text", "text": "A"}],
                  "stop_reason": "end_turn"},
    "gemini": {"candidates": [{"content": {"parts": [
        {"text": "hmm", "thought": True}, {"text": "A"}]},
        "finishReason": "STOP"}]},
}

TRUNCATED = {
    "openai": {"choices": [{"message": {"content": "A"},
                            "finish_reason": "length"}]},
    "responses": {"status": "incomplete",
                  "incomplete_details": {"reason": "max_output_tokens"},
                  "output": [{"type": "message", "content": [
                      {"type": "output_text", "text": "A"}]}]},
    "anthropic": {"content": [{"type": "text", "text": "A"}],
                  "stop_reason": "max_tokens"},
    "gemini": {"candidates": [{"content": {"parts": [{"text": "A"}]},
                               "finishReason": "MAX_TOKENS"}]},
}


class QuietServer(ThreadingHTTPServer):
    daemon_threads = True
    request_queue_size = 128

    def handle_error(self, request, client_address):
        if not isinstance(sys.exc_info()[1], (ConnectionError, OSError)):
            super().handle_error(request, client_address)


def sse_of(payload):
    """The events of a streamed answer equal to the whole answer @p payload,
    in its protocol, each piece of text in up to three deltas: (event name or
    None, data) pairs."""
    def pieces(text):
        cut = [0, len(text) // 3, 2 * len(text) // 3, len(text)]
        return [text[a:b] for a, b in zip(cut, cut[1:]) if b > a] or [text]

    events = []
    if "error" in payload:
        events.append(("error", payload))
    elif "choices" in payload:
        for choice in payload["choices"][:1]:
            message = choice.get("message", {})
            events.append((None, {"choices": [{"index": 0, "delta": {
                "role": "assistant", "content": ""}}]}))
            if message.get("reasoning_content"):
                events.append((None, {"choices": [{"index": 0, "delta": {
                    "reasoning_content": message["reasoning_content"]}}]}))
            content = message.get("content")
            texts = [content] if isinstance(content, str) else \
                [p["text"] for p in content or []]
            for text in texts:
                for piece in pieces(text):
                    events.append((None, {"choices": [{"index": 0, "delta": {
                        "content": piece}}]}))
            if message.get("refusal"):
                events.append((None, {"choices": [{"index": 0, "delta": {
                    "refusal": message["refusal"]}}]}))
            events.append((None, {"choices": [{"index": 0, "delta": {},
                                               "finish_reason":
                                               choice.get("finish_reason")}]}))
        events.append((None, "[DONE]"))
    elif "output" in payload:
        events.append(("response.created", {"type": "response.created"}))
        for item in payload["output"]:
            for part in item.get("content", []) if item["type"] == "reasoning" else []:
                for piece in pieces(part["text"]):
                    events.append(("response.reasoning_text.delta", {
                        "type": "response.reasoning_text.delta", "delta": piece}))
            for part in item.get("content", []) if item["type"] == "message" else []:
                kind = "refusal" if part["type"] == "refusal" else "output_text"
                for piece in pieces(part.get("refusal") or part.get("text") or ""):
                    events.append((f"response.{kind}.delta", {
                        "type": f"response.{kind}.delta", "delta": piece}))
        done = "response.incomplete" if payload.get("status") == "incomplete" \
            else "response.completed"
        events.append((done, {"type": done, "response": payload}))
    elif "content" in payload:
        events.append(("message_start", {"type": "message_start", "message": {
            "content": [], "stop_reason": None}}))
        for i, block in enumerate(payload["content"]):
            kind = block["type"]
            events.append(("content_block_start", {
                "type": "content_block_start", "index": i,
                "content_block": {"type": kind, kind: ""}}))
            for piece in pieces(block.get(kind, "")):
                events.append(("content_block_delta", {
                    "type": "content_block_delta", "index": i,
                    "delta": {"type": f"{kind}_delta", kind: piece}}))
            events.append(("content_block_stop", {"type": "content_block_stop",
                                                  "index": i}))
        events.append(("message_delta", {"type": "message_delta", "delta": {
            "stop_reason": payload.get("stop_reason")}}))
        events.append(("message_stop", {"type": "message_stop"}))
    else:
        for candidate in payload.get("candidates", [])[:1]:
            for part in candidate.get("content", {}).get("parts", []):
                for piece in pieces(part["text"]):
                    events.append((None, {"candidates": [{"content": {
                        "role": "model", "parts": [dict(part, text=piece)]}}]}))
            events.append((None, {"candidates": [{"content": {
                "role": "model", "parts": []},
                "finishReason": candidate.get("finishReason")}]}))
        if "promptFeedback" in payload:
            events.append((None, {"promptFeedback": payload["promptFeedback"]}))
    return events


def sse_wire(events, lines=False):
    """Events as an event stream's bytes; with @p lines, each JSON line on a
    data line of its own."""
    out = []
    for name, data in events:
        text = data if isinstance(data, str) else json.dumps(
            data, indent=1 if lines else None)
        out.append((f"event: {name}\n" if name else "") +
                   "".join(f"data: {line}\n" for line in text.split("\n")) + "\n")
    return "".join(out).encode()


class Stream:
    """A streamed answer: @p wire's bytes, or else the events of @p payload
    (sse_of()), written in pieces of the @p sizes in turn (all at once without
    them), @p gap seconds apart, and then @p stall seconds of silence before
    the end."""

    def __init__(self, payload=None, wire=None, sizes=(), gap=0, stall=0):
        self.wire = wire if wire is not None else sse_wire(sse_of(payload))
        self.sizes = sizes
        self.gap = gap
        self.stall = stall

    def pieces(self):
        at = 0
        for size in self.sizes:
            if at >= len(self.wire):
                return
            yield self.wire[at:at + size]
            at += size
        if at < len(self.wire):
            yield self.wire[at:]


class Upstream:
    """Fake AI endpoint; reply(request) gives (status, payload[, delay]).  A
    request that asks for a stream, as askai's do, gets a 200's JSON payload
    as the events of the same answer, unless plain is set, and a Stream as
    it says; other answers carry the fields in headers.  GET /props gives
    props, if set, else a 404, two seconds late if props is "hang", and
    counts in gets."""

    def __init__(self):
        self.requests = []
        self.reply = lambda request: (200, ANSWERS["openai"])
        self.plain = False
        self.props = None
        self.headers = {}
        self.gets = 0
        self.arrived = threading.Event()
        self.cancelled = threading.Event()
        self.hangups = 0
        self.lock = threading.Lock()
        upstream = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, fmt, *args):
                pass

            def do_GET(self):
                with upstream.lock:
                    upstream.gets += 1
                props = upstream.props
                if props == "hang":
                    time.sleep(2)
                    props = None
                wire = json.dumps(props).encode() if props is not None else b""
                self.send_response(200 if props is not None else 404)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(wire)))
                self.end_headers()
                self.wfile.write(wire)

            def do_POST(self):
                raw = self.rfile.read(int(self.headers["Content-Length"]))
                request = {
                    "path": self.path,
                    "headers": {k.lower(): v for k, v in self.headers.items()},
                    "raw": raw,
                    "body": json.loads(raw),
                }
                with upstream.lock:
                    upstream.requests.append(request)
                upstream.arrived.set()
                status, payload, *delay = upstream.reply(request)
                if delay and upstream.wait_for_hangup(self.connection, delay[0]):
                    upstream.hung_up()
                    return
                streamed = request["body"].get("stream") is True or \
                    "streamGenerateContent" in self.path
                if streamed and status == 200 and isinstance(payload, dict) and \
                        not upstream.plain:
                    payload = Stream(payload)
                if isinstance(payload, Stream):
                    self.stream(payload)
                    return
                wire = payload if isinstance(payload, bytes) else \
                    json.dumps(payload).encode()
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(wire)))
                for name, value in upstream.headers.items():
                    self.send_header(name, value)
                self.end_headers()
                self.wfile.write(wire)

            def stream(self, answer):
                self.send_response(200)
                self.send_header("Content-Type", "text/event-stream")
                self.send_header("Transfer-Encoding", "chunked")
                self.end_headers()
                try:
                    for i, piece in enumerate(answer.pieces()):
                        if i and answer.gap and upstream.wait_for_hangup(
                                self.connection, answer.gap):
                            raise ConnectionResetError
                        self.wfile.write(b"%x\r\n%s\r\n" % (len(piece), piece))
                        self.wfile.flush()
                    if answer.stall and upstream.wait_for_hangup(self.connection,
                                                                 answer.stall):
                        raise ConnectionResetError
                    self.wfile.write(b"0\r\n\r\n")
                    self.wfile.flush()
                except (ConnectionError, OSError):
                    upstream.hung_up()
                    self.close_connection = True

        self.server = QuietServer(("127.0.0.1", 0), Handler)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def hung_up(self):
        with self.lock:
            self.hangups += 1
        self.cancelled.set()

    @staticmethod
    def wait_for_hangup(conn, seconds):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            readable, _, _ = select.select([conn], [], [], 0.02)
            try:
                if readable and conn.recv(1, socket.MSG_PEEK) == b"":
                    return True
            except ConnectionError:
                return True
        return False

    def url(self, path="/v1/chat/completions"):
        return f"http://127.0.0.1:{self.server.server_port}{path}"

    def last(self):
        with self.lock:
            return self.requests[-1]

    def close(self):
        self.server.shutdown()
        self.server.server_close()


class Askai:
    """An askai process on an ephemeral port; stop() expects a clean exit.
    It serves clients on that many loops, or with loops=None as many as it
    has CPUs.  With nofile, it starts with that descriptor limit, soft and
    hard; with preload, with that library preloaded; with ignore, with those
    signals ignored; with data, with that data segment limit."""

    def __init__(self, binary, *args, loops=4, env=None, nofile=None,
                 preload=None, ignore=(), data=None):
        child_env = {k: v for k, v in os.environ.items() if k not in KEY_VARS}
        child_env.update(env or {})
        if preload:
            # A sanitizer runtime insists on coming first otherwise.
            child_env["LD_PRELOAD"] = preload
            child_env["ASAN_OPTIONS"] = ":".join(filter(None, (
                child_env.get("ASAN_OPTIONS"), "verify_asan_link_order=0")))
        def setup():
            if nofile:
                resource.setrlimit(resource.RLIMIT_NOFILE, (nofile, nofile))
            if data:
                resource.setrlimit(resource.RLIMIT_DATA, (data, data))
            for sig in ignore:
                signal.signal(sig, signal.SIG_IGN)

        if loops is not None:
            args = ("--loops", str(loops), *args)
        self.proc = subprocess.Popen(
            [binary, "-l", "127.0.0.1:0", *args], env=child_env,
            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE, text=True,
            preexec_fn=setup if nofile or ignore or data else None)
        # Lines may come before the endpoint's, such as memory pressure's.
        self.log = []
        for line in self.proc.stderr:
            match = re.search(r"http://([\d.]+):(\d+)/mcp$", line.strip())
            if match:
                break
            self.log.append(line)
        else:
            self.proc.kill()
            raise AssertionError("askai did not start: " + "".join(self.log))
        self.host = match.group(1)
        self.port = int(match.group(2))
        threading.Thread(target=self.drain, daemon=True).start()
        # Home knows the upstream's slots, from /props or not, before it
        # makes connections of its own.
        self.wait_log(r"upstream slots: ")

    def drain(self):
        for line in self.proc.stderr:
            self.log.append(line)

    def loops(self):
        """The names of its event loops' threads, home's last: once a request
        has been served, every loop runs."""
        task = f"/proc/{self.proc.pid}/task"
        names = []
        for tid in os.listdir(task):
            with open(f"{task}/{tid}/comm") as f:
                names.append(f.read().strip())
        return sorted((n for n in names if re.fullmatch(r"loop\d+", n)),
                      key=lambda n: int(n[4:])) + ["home"]

    def connect(self):
        return http.client.HTTPConnection(self.host, self.port, timeout=10)

    def get(self, path, headers=None, source=None):
        """(status, Content-Type, text) of a GET, from @p source if given."""
        conn = http.client.HTTPConnection(self.host, self.port, timeout=10,
                                          source_address=source)
        conn.request("GET", path, headers=headers or {})
        response = conn.getresponse()
        text = response.read().decode()
        conn.close()
        return response.status, response.getheader("Content-Type"), text

    def metrics(self, headers=None):
        """GET /metrics as {series: value}, checked line by line."""
        status, ctype, text = self.get("/metrics", headers)
        assert status == 200, (status, text)
        assert ctype == "text/plain; version=0.0.4; charset=utf-8", ctype
        return prometheus(text)

    def wait_log(self, pattern, timeout=5):
        """The first line of the log that matches @p pattern, waiting for it."""
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            for line in list(self.log):
                if re.search(pattern, line):
                    return line
            time.sleep(0.01)
        raise AssertionError(f"no {pattern!r} in the log: " + "".join(self.log))

    def post(self, body, headers=None, conn=None):
        own = conn is None
        conn = conn or self.connect()
        all_headers = {"Content-Type": "application/json",
                       "Accept": "application/json, text/event-stream"}
        all_headers.update(headers or {})
        wire = body if isinstance(body, bytes) else json.dumps(body).encode()
        conn.request("POST", "/mcp", body=wire, headers=all_headers)
        response = conn.getresponse()
        raw = response.read()
        if own:
            conn.close()
        headers = {k.lower(): v for k, v in response.getheaders()}
        if headers.get("content-type") == "text/event-stream":
            events, _ = sse_events(raw)
            assert len(events) <= 1, events
            return response.status, headers, \
                json.loads(events[0]) if events else None
        return response.status, headers, json.loads(raw) if raw else None

    def rpc(self, method, params=None, rpc_id=1, headers=None, conn=None):
        msg = {"jsonrpc": "2.0", "method": method}
        if rpc_id is not None:
            msg["id"] = rpc_id
        if params is not None:
            msg["params"] = params
        return self.post(msg, headers, conn)

    def modern(self, method, params=None, rpc_id=1, headers=None, meta=None):
        params = dict(params or {})
        params["_meta"] = meta if meta is not None else {
            "io.modelcontextprotocol/protocolVersion": MODERN,
            "io.modelcontextprotocol/clientCapabilities": {},
        }
        all_headers = {"MCP-Protocol-Version": MODERN, "Mcp-Method": method}
        if "name" in params or "taskId" in params:
            all_headers["Mcp-Name"] = params.get("name", params.get("taskId"))
        all_headers.update(headers or {})
        return self.rpc(method, params, rpc_id, all_headers)

    def ask(self, prompt="Q", **arguments):
        arguments["prompt"] = prompt
        status, _, body = self.rpc("tools/call",
                                   {"name": "ask_ai", "arguments": arguments})
        assert status == 200, (status, body)
        return body["result"]

    def raw(self, data, read_until_close=True):
        with socket.create_connection(("127.0.0.1", self.port), 10) as sock:
            sock.sendall(data)
            chunks = []
            while read_until_close:
                chunk = sock.recv(65536)
                if not chunk:
                    break
                chunks.append(chunk)
            return b"".join(chunks)

    def stop(self):
        self.proc.send_signal(signal.SIGTERM)
        assert self.proc.wait(timeout=5) == 0, "".join(self.log)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        if exc[0] is None:
            self.stop()
        else:
            self.proc.kill()
            self.proc.wait()
            sys.stderr.write("".join(self.log))


class Replies:
    """HTTP/1.1 replies with a Content-Length, read off a raw socket."""

    def __init__(self, sock):
        self.sock = sock
        self.buf = bytearray()

    def fill(self, size):
        while len(self.buf) < size:
            chunk = self.sock.recv(65536)
            if not chunk:
                return False
            self.buf += chunk
        return True

    def next(self):
        """(status, JSON body or None) of the next reply; None once the peer
        closed."""
        while (end := self.buf.find(b"\r\n\r\n")) < 0:
            if not self.fill(len(self.buf) + 1):
                return None
        head = bytes(self.buf[:end])
        length = int(re.search(rb"\r\nContent-Length: (\d+)", head).group(1))
        assert self.fill(end + 4 + length), "truncated reply"
        body = bytes(self.buf[end + 4:end + 4 + length])
        del self.buf[:end + 4 + length]
        return int(head[9:12]), json.loads(body) if body else None


def sse_events(raw):
    """The data of each event in an event stream, and its comments."""
    events, comments, data = [], [], []
    for line in raw.decode().split("\n"):
        if line.startswith(":"):
            comments.append(line)
        elif line.startswith("data:"):
            data.append(line[5:].removeprefix(" "))
        elif not line and data:
            events.append("\n".join(data))
            data = []
        else:
            assert not line, line
    assert not data, "an unterminated event"
    return events, comments


def prometheus(text):
    """Prometheus text exposition as {series: value}: each family has its
    HELP and TYPE, and each histogram's buckets rise to its count."""
    values, types = {}, {}
    for line in text.splitlines():
        if line.startswith("# HELP "):
            continue
        if line.startswith("# TYPE "):
            _, _, name, kind = line.split()
            assert kind in ("counter", "gauge", "histogram"), line
            types[name] = kind
            continue
        match = re.fullmatch(r'([a-z_]+)(\{[a-z_]+="[^"]*"(,[a-z_]+="[^"]*")*\})? '
                             r'(\d+(\.\d+)?(e[-+]?\d+)?)', line)
        assert match, line
        name = match.group(1)
        family = re.sub(r"_(bucket|sum|count)$", "", name)
        assert name in types or types.get(family) == "histogram", line
        values[name + (match.group(2) or "")] = float(match.group(4))
    for series, value in values.items():
        if "_bucket{" in series and 'le="+Inf"' in series:
            labels = re.sub(r',?le="\+Inf"', "", series.split("{", 1)[1])
            base = series.split("_bucket{")[0]
            count = base + "_count" + ("{" + labels if labels != "}" else "")
            assert values[count] == value, (series, value, values.get(count))
            buckets = [v for k, v in values.items()
                       if k.startswith(base + "_bucket{" + labels.rstrip("}"))]
            assert buckets == sorted(buckets), (series, buckets)
    return values


def text_of(result):
    assert [c["type"] for c in result["content"]] == ["text"], result
    return result["content"][0]["text"]


def total(values, name, **labels):
    """The sum of the series @p name with @p labels, over its other labels,
    such as each loop's."""
    found = 0
    for series, value in values.items():
        base, _, rest = series.partition("{")
        have = dict(re.findall(r'(\w+)="([^"]*)"', rest))
        if base == name and all(have.get(k) == v for k, v in labels.items()):
            found += value
    return found


def check_legacy_session(binary, up):
    with Askai(binary, "-u", up.url(), "-m", "m",
               env={"OPENAI_API_KEY": "must-not-leak"}) as ai:
        status, headers, body = ai.rpc("initialize", {
            "protocolVersion": LEGACY, "capabilities": {},
            "clientInfo": {"name": "test", "version": "1"}})
        assert status == 200
        assert re.fullmatch(r"[0-9a-f]{32}", headers["mcp-session-id"])
        assert body["result"]["protocolVersion"] == LEGACY
        assert body["result"]["capabilities"] == {"tools": {}}
        assert body["result"]["serverInfo"]["name"] == "askai"
        assert "resultType" not in body["result"]
        for asked, granted in (("2024-11-05", "2024-11-05"), ("1999-01-01", LEGACY)):
            _, _, body = ai.rpc("initialize", {"protocolVersion": asked})
            assert body["result"]["protocolVersion"] == granted

        hdr = {"MCP-Protocol-Version": LEGACY}
        status, _, body = ai.rpc("notifications/initialized", rpc_id=None,
                                 headers=hdr)
        assert status == 202 and body is None
        assert ai.rpc("ping", headers=hdr)[2] == {"jsonrpc": "2.0", "id": 1,
                                                 "result": {}}
        _, _, body = ai.rpc("tools/list", headers=hdr)
        tool, = body["result"]["tools"]
        assert tool["name"] == "ask_ai" and "m" in tool["description"]
        assert tool["inputSchema"]["required"] == ["prompt"]

        assert text_of(ai.ask("What is 6*7?")) == "A"
        request = up.last()
        assert request["body"] == {"model": "m", "stream": True, "messages": [
            {"role": "user", "content": "What is 6*7?"}]}
        assert "authorization" not in request["headers"], "key leaked to -u URL"
        assert request["headers"]["user-agent"].startswith("askai/")

        text_of(ai.ask("Q", system="Be brief."))
        assert up.last()["body"]["messages"][0] == {"role": "system",
                                                   "content": "Be brief."}

        for arguments in ({}, {"prompt": ""}, {"prompt": 1},
                          {"prompt": "Q", "system": 2}):
            status, _, body = ai.rpc("tools/call", {"name": "ask_ai",
                                                    "arguments": arguments})
            assert status == 200 and body["result"]["isError"] is True, body

        status, _, body = ai.rpc("tools/call", {"name": "nope"}, rpc_id="x")
        assert status == 200 and body["error"]["code"] == -32602
        assert body["id"] == "x"
        status, _, body = ai.rpc("resources/list")
        assert status == 200 and body["error"]["code"] == -32601
        status, _, body = ai.post({"jsonrpc": "2.0", "id": 9, "result": {}})
        assert status == 202 and body is None

    print("legacy initialize/tools session: PASS")


def check_protocols(binary, up):
    # Every protocol asks for a streamed answer, which --extra cannot turn
    # off; gemini by its URL.
    expect = {
        "openai": ("/v1/chat/completions", "authorization", "Bearer secret", {
            "model": "m", "max_tokens": 77, "temperature": 0.25, "top_p": 0.5,
            "stream": True, "messages": [{"role": "system", "content": "SYS"},
                                         {"role": "user", "content": "Q"}]}),
        "responses": ("/v1/responses", "authorization", "Bearer secret", {
            "model": "m", "input": [{"role": "user", "content": "Q"}],
            "instructions": "SYS", "store": False,
            "max_output_tokens": 77, "temperature": 0.25, "top_p": 0.5,
            "stream": True}),
        "anthropic": ("/v1/messages", "x-api-key", "secret", {
            "model": "m", "max_tokens": 77, "temperature": 0.25, "top_p": 0.5,
            "stream": True, "system": "SYS",
            "messages": [{"role": "user", "content": "Q"}]}),
        "gemini": ("/v1beta/models/m:generateContent", "x-goog-api-key", "secret", {
            "contents": [{"role": "user", "parts": [{"text": "Q"}]}],
            "systemInstruction": {"parts": [{"text": "SYS"}]},
            "generationConfig": {"maxOutputTokens": 77, "temperature": 0.25},
            "top_p": 0.5, "stream": False}),
    }
    for protocol, (path, auth, value, body) in expect.items():
        with Askai(binary, "--api", API[protocol], "-u", up.url(path), "-m", "m",
                   "-k", "TEST_KEY", "-s", "SYS", "-n", "77", "-t", "0.25",
                   "-x", '{"top_p": 0.5, "store": null, "stream": false}',
                   env={"TEST_KEY": "secret"}) as ai:
            up.reply = lambda request, p=protocol: (200, ANSWERS[p])
            assert text_of(ai.ask()) == "A", protocol
            request = up.last()
            if protocol == "gemini":
                path = "/v1beta/models/m:streamGenerateContent?alt=sse"
            assert request["path"] == path, (protocol, request["path"])
            assert request["headers"][auth] == value, protocol
            if protocol == "responses":
                body = dict(body)
                del body["store"]  # --extra null deletes keys
            assert request["body"] == body, (protocol, request["body"])
            assert b'"temperature":0.25' in request["raw"], request["raw"]
            assert (request["headers"].get("anthropic-version") ==
                    ("2023-06-01" if protocol == "anthropic" else None))

            up.reply = lambda request, p=protocol: (200, TRUNCATED[p])
            result = ai.ask()
            assert "isError" not in result
            assert text_of(result).startswith("A\n\n[The answer was cut short")

    with Askai(binary, "--api", "messages", "-u", up.url("/v1/messages")) as ai:
        up.reply = lambda request: (200, ANSWERS["anthropic"])
        ai.ask()
        assert up.last()["body"]["max_tokens"] == 4096
        assert "model" not in up.last()["body"]

    # A Gemini URL's alt, whatever it was, is sse.
    for query, want in (("?alt=json&x=1", "?alt=sse&x=1"),
                        ("?x=1&alt=json", "?x=1&alt=sse"), ("?x=1", "?x=1&alt=sse")):
        with Askai(binary, "--api", "generate",
                   "-u", up.url("/v1beta/models/m:generateContent" + query)) as ai:
            up.reply = lambda request: (200, ANSWERS["gemini"])
            assert text_of(ai.ask()) == "A"
            assert up.last()["path"] == "/v1beta/models/m:streamGenerateContent" + want

    up.reply = lambda request: (200, ANSWERS["openai"])
    print("openai/responses/anthropic/gemini requests and answers: PASS")


def check_upstream_errors(binary, up):
    cases = [
        ((401, {"error": {"message": "bad key", "type": "auth"}}),
         "Upstream error (HTTP 401): bad key"),
        ((502, b"<html>Bad gateway</html>"),
         "Upstream error (HTTP 502): <html>Bad gateway</html>"),
        ((200, {"error": "overloaded"}), "Upstream error (HTTP 200): overloaded"),
        ((200, {"choices": []}), "The model returned no answer."),
        ((200, {"choices": [{"message": {"content": None},
                             "finish_reason": "content_filter"}]}),
         "The model returned no answer (stop reason: content_filter)."),
        ((200, {"choices": [{"message": {"content": None, "refusal": "No."}}]}),
         None),
        # Only UTF-8 text may enter a tool result.
        ((502, b"bad \xe2\x82 gateway"), "Upstream error (HTTP 502): bad "),
        ((502, b"\xff"),
         "Upstream error (HTTP 502): a response that is not text"),
        ((200, b'{"choices":[{"message":{"content":"\xc0\xa0"}}]}'),
         'Upstream error (HTTP 200): {"choices":[{"message":{"content":"'),
        ((502, b"\xe2\x82\xac" * 171),
         "Upstream error (HTTP 502): " + "\u20ac" * 170),
    ]
    with Askai(binary, "-u", up.url()) as ai:
        for reply, message in cases:
            up.reply = lambda request, r=reply: r
            result = ai.ask()
            if message is None:
                assert text_of(result) == "No." and "isError" not in result
            else:
                assert result["isError"] is True and text_of(result) == message, \
                    (text_of(result), message)

    with Askai(binary, "-u", up.url(), "-T", "1") as ai:
        up.reply = lambda request: (200, ANSWERS["openai"], 3)
        result = ai.ask("SLOW")
        assert result["isError"] and "timed out after 1 s" in text_of(result)
    up.reply = lambda request: (200, ANSWERS["openai"])

    with Askai(binary, "-u", "http://127.0.0.1:1/v1/chat/completions") as ai:
        result = ai.ask()
        assert result["isError"] and "Upstream request failed" in text_of(result)

    print("upstream errors, refusals, timeouts: PASS")


def check_modern(binary, up):
    with Askai(binary, "-u", up.url()) as ai:
        status, headers, body = ai.modern("server/discover")
        assert status == 200 and "mcp-session-id" not in headers
        result = body["result"]
        assert result["supportedVersions"][0] == MODERN
        assert LEGACY in result["supportedVersions"]
        assert result["capabilities"] == {
            "tools": {}, "extensions": {"io.modelcontextprotocol/tasks": {}}}
        assert result["resultType"] == "complete"
        assert result["_meta"]["io.modelcontextprotocol/serverInfo"]["name"] == "askai"
        assert result["cacheScope"] == "public" and result["ttlMs"] > 0

        _, _, body = ai.modern("tools/list")
        assert body["result"]["tools"][0]["name"] == "ask_ai"
        assert body["result"]["resultType"] == "complete" and "ttlMs" in body["result"]

        status, _, body = ai.modern("tools/call", {"name": "ask_ai",
                                                   "arguments": {"prompt": "Q"}})
        assert status == 200 and text_of(body["result"]) == "A"
        assert body["result"]["resultType"] == "complete"

        def error(expected_status, code, *args, **kwargs):
            status, _, body = ai.modern(*args, rpc_id=7, **kwargs)
            assert (status, body["error"]["code"], body["id"]) == \
                (expected_status, code, 7), (status, body)
            return body["error"]

        error(400, -32020, "tools/list", headers={"MCP-Protocol-Version": LEGACY})
        error(400, -32020, "tools/list", headers={"Mcp-Method": "prompts/list"})
        error(400, -32020, "tools/list", headers={"Mcp-Method": "TOOLS/LIST"})
        error(400, -32020, "tools/call", {"name": "ask_ai"},
              headers={"Mcp-Name": "other"})
        error(400, -32602, "tools/list", meta={
            "io.modelcontextprotocol/protocolVersion": MODERN})
        data = error(400, -32022, "tools/list",
                     headers={"MCP-Protocol-Version": "2099-01-01"},
                     meta={"io.modelcontextprotocol/protocolVersion": "2099-01-01",
                           "io.modelcontextprotocol/clientCapabilities": {}})["data"]
        assert data["requested"] == "2099-01-01" and MODERN in data["supported"]
        for removed in ("initialize", "ping", "prompts/list"):
            error(404, -32601, removed)

        status, _, body = ai.modern("tools/call", {"name": "ask_ai",
                                                   "arguments": {"prompt": "Q"}},
                                    headers={"Mcp-Name": "  ask_ai  "})
        assert status == 200 and text_of(body["result"]) == "A"

    print("2026-07-28 stateless requests and header validation: PASS")


def check_mcp_tasks(binary, up):
    """MCP's tasks extension (2026-07-28): a tool call of a client that takes
    tasks becomes one once it has run a second, and its client polls it
    with tasks/get until the result is in, which only its own identity
    finds, or ends it with tasks/cancel.  A call that answers sooner, and
    one of a client that does not take tasks, answer as ever."""
    def digest(token):
        return "sha256:" + hashlib.sha256(token.encode()).hexdigest()

    alice, bob = ({"Authorization": f"Bearer {t}"} for t in ("a-1", "b-1"))
    iso = r"\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3}Z"
    hold = up.reply
    with tempfile.TemporaryDirectory() as d:
        path = f"{d}/askai.json"
        with open(path, "w") as f:
            json.dump({"url": up.url(),
                       "roles": {"asker": {"tools": ["ask_ai"], "metrics": True}},
                       "identities": {"alice": {"role": "asker", "tokens": [digest("a-1")]},
                                      "bob": {"role": "asker", "tokens": [digest("b-1")]}}},
                      f)
        os.chmod(path, 0o600)
        try:
            with Askai(binary, "-c", path) as ai:
                def call(delay, meta=TAKES_TASKS):
                    up.reply = lambda r: (200, ANSWERS["openai"], delay)
                    start = time.monotonic()
                    status, _, body = ai.modern("tools/call", {
                        "name": "ask_ai", "arguments": {"prompt": "Q"}}, meta=meta,
                        headers=alice)
                    assert status == 200, body
                    return body["result"], time.monotonic() - start

                def ask(task_id, method="tasks/get", meta=TAKES_TASKS, headers=alice):
                    return ai.modern(method, {"taskId": task_id}, meta=meta,
                                     headers=headers)

                def poll(task_id):
                    for _ in range(100):
                        result = ask(task_id)[2]["result"]
                        if result["status"] != "working":
                            return result
                        time.sleep(0.1)
                    raise AssertionError(result)

                # Calls that answer within the second, or of clients that do not
                # take tasks, answer as ever.
                for delay, meta, at_least in ((0, TAKES_TASKS, 0), (1.5, None, 1.5)):
                    result, took = call(delay, meta)
                    assert result["resultType"] == "complete" and text_of(result) == "A" \
                        and took >= at_least, (result, took)

                # A slow one is a task after a second, whose record its client
                # polls until the call's result is in.
                task, took = call(2)
                assert 0.9 <= took < 1.8, took
                tid = task.pop("taskId")
                assert re.fullmatch(r"[0-9a-f]{32}", tid) and \
                    re.fullmatch(iso, task["createdAt"]), task
                assert task.pop("_meta")["io.modelcontextprotocol/serverInfo"]["name"] == \
                    "askai" and task == {"resultType": "task", "status": "working",
                                         "createdAt": task["createdAt"],
                                         "lastUpdatedAt": task["createdAt"],
                                         "ttlMs": None, "pollIntervalMs": 1000}, task
                status, _, body = ask(tid)
                assert status == 200 and body["result"]["status"] == "working" and \
                    body["result"]["resultType"] == "complete" and \
                    "result" not in body["result"], body
                done = poll(tid)
                assert done["status"] == "completed" and text_of(done["result"]) == "A" \
                    and done["result"]["resultType"] == "complete", done
                assert done["ttlMs"] >= 600_000 and done["createdAt"] == task["createdAt"] \
                    and done["createdAt"] < done["lastUpdatedAt"], done

                # Another identity's, an unknown id, a client that does not take
                # tasks, a mismatched Mcp-Name, and a legacy client.
                for args, kwargs, expected in (
                        ((tid,), {"headers": bob}, (200, -32602)),
                        (("0" * 32,), {}, (200, -32602)),
                        ((tid[:-1],), {}, (200, -32602)),
                        ((tid,), {"meta": None}, (400, -32021)),
                        ((tid,), {"headers": alice | {"Mcp-Name": "x"}}, (400, -32020))):
                    status, _, body = ask(*args, **kwargs)
                    assert (status, body["error"]["code"]) == expected, (args, body)
                assert body["error"]["message"] == \
                    "Mcp-Name header does not match the task id", body
                assert ask(tid, meta=None)[2]["error"]["data"] == {"requiredCapabilities": {
                    "extensions": {"io.modelcontextprotocol/tasks": {}}}}
                status, _, body = ai.rpc("tasks/get", {"taskId": tid}, headers=alice)
                assert body["error"]["code"] == -32601, body

                # tasks/update has no input to give; tasks/cancel of an ended task
                # changes nothing.
                for method in ("tasks/update", "tasks/cancel"):
                    result = ask(tid, method)[2]["result"]
                    assert result.pop("resultType") == "complete" and \
                        list(result) == ["_meta"], result
                assert ask(tid)[2]["result"] == done | {"_meta": done["_meta"]}

                # tasks/cancel ends a working task's call, its upstream call
                # with it.
                up.cancelled.clear()
                task, _ = call(30)
                assert ask(task["taskId"], "tasks/cancel")[0] == 200
                assert up.cancelled.wait(5)
                gone = poll(task["taskId"])
                assert gone["status"] == "cancelled" and "result" not in gone and \
                    gone["statusMessage"], gone

                values = ai.metrics(alice)
                assert values["askai_mcp_tasks_opened_total"] == 2 and \
                    values['askai_mcp_tasks_ended_total{status="completed"}'] == 1 and \
                    values['askai_mcp_tasks_ended_total{status="cancelled"}'] == 1 and \
                    values["askai_mcp_tasks_kept"] == 2 and \
                    values["askai_mcp_task_result_bytes"] > 0, values
                assert total(values, "askai_request_seconds_count", method="tasks/get") >= 4
        finally:
            up.reply = hold
    print("MCP tasks: calls that run a second become tasks, polled, bound to their "
          "identity, cancelled: PASS")


def check_http(binary, up):
    with Askai(binary, "-u", up.url(), "-o", "https://ui.example") as ai:
        conn = ai.connect()
        conn.request("OPTIONS", "/mcp", headers={
            "Origin": "http://localhost:8080",
            "Access-Control-Request-Method": "POST",
            "Access-Control-Request-Headers": "content-type, mcp-session-id"})
        response = conn.getresponse()
        response.read()
        assert response.status == 204
        assert response.getheader("Access-Control-Allow-Origin") == \
            "http://localhost:8080"
        assert response.getheader("Access-Control-Allow-Headers") == \
            "content-type, mcp-session-id"
        assert "POST" in response.getheader("Access-Control-Allow-Methods")
        assert response.getheader("Content-Length") is None

        for origin in ("http://127.0.0.1:8080", "http://[::1]", "https://ui.example"):
            status, headers, _ = ai.rpc("ping", headers={"Origin": origin},
                                        conn=conn)
            assert status == 200 and headers["access-control-allow-origin"] == origin
            assert "Mcp-Session-Id" in headers["access-control-expose-headers"]
        conn.close()

        for origin in ("http://evil.example", "null", "http://localhost.evil:80",
                       "http://localhost:99999x"):
            status, headers, _ = ai.rpc("ping", headers={"Origin": origin})
            assert status == 403 and "access-control-allow-origin" not in headers

        for method in ("GET", "DELETE"):
            conn = ai.connect()
            conn.request(method, "/mcp", headers={"Accept": "text/event-stream"})
            response = conn.getresponse()
            response.read()
            assert response.status == 405 and "POST" in response.getheader("Allow")
            conn.close()

        assert ai.rpc("ping", headers={"Content-Type": "text/plain"})[0] == 415
        status, _, body = ai.post(b"{not json")
        assert status == 400 and body["error"]["code"] == -32700
        status, _, body = ai.post([{"jsonrpc": "2.0", "id": 1, "method": "ping"}])
        assert status == 400 and body["error"]["code"] == -32600

        conn = ai.connect()
        for rpc_id in range(3):
            assert ai.rpc("ping", rpc_id=rpc_id, conn=conn)[2]["id"] == rpc_id
        conn.close()

        ping = b'{"jsonrpc":"2.0","id":%d,"method":"ping"}'
        length = b"Content-Length: %d\r\n" % len(ping % 0)
        request = (b"POST /mcp HTTP/1.1\r\nHost: x\r\n"
                   b"Content-Type: application/json\r\n" + length + b"\r\n")
        wire = ai.raw(request + ping % 1 + request + ping % 2 +
                      b"POST /mcp HTTP/1.1\r\nConnection: close\r\n"
                      b"Content-Type: application/json\r\n" + length + b"\r\n" +
                      ping % 3)
        assert [int(i) for i in re.findall(rb'"id":(\d)', wire)] == [1, 2, 3], wire

        with socket.create_connection(("127.0.0.1", ai.port), 10) as sock:
            sock.sendall(b"POST /mcp HTTP/1.1\r\nContent-Type: application/json\r\n" +
                         length + b"Expect: 100-continue\r\n\r\n")
            assert sock.recv(4096) == b"HTTP/1.1 100 Continue\r\n\r\n"
            sock.sendall(ping % 4)
            assert b'"id":4' in sock.recv(4096)

        def status_of(wire):
            match = re.match(rb"HTTP/1\.1 (\d+) ", ai.raw(wire))
            return int(match.group(1)) if match else None

        assert status_of(b"POST /mcp HTTP/1.1 junk\r\n\r\n") == 400
        assert status_of(b"POST /mcp HTTP/2.0\r\n\r\n") == 505
        assert status_of(b"POST /mcp HTTP/1.1\r\nBad Header: x\r\n\r\n") == 400
        assert status_of(b"POST /mcp HTTP/1.1\r\n folded: x\r\n\r\n") == 400
        assert status_of(b"POST /mcp HTTP/1.1\r\nX: " + b"a" * 20000 +
                         b"\r\n\r\n") == 431
        assert status_of(b"POST /mcp HTTP/1.1\r\nContent-Length: 99999999\r\n\r\n") == 413
        assert status_of(b"POST /mcp HTTP/1.1\r\nContent-Length: -1\r\n\r\n") == 400
        assert status_of(b"POST /mcp HTTP/1.1\r\nContent-Length: 2\r\n"
                         b"Content-Length: 2\r\n\r\n{}") == 400
        # Chunked bodies (RFC 9112 7.1), with extensions and trailers, and a
        # request pipelined after one.
        chunked = (b"POST /mcp HTTP/1.1\r\nContent-Type: application/json\r\n"
                   b"Transfer-Encoding: chunked\r\n\r\n")
        body = ping % 6
        wire = ai.raw(chunked + b"5\r\n" + body[:5] + b"\r\n" +
                      b"0000A ; ext=1;x\r\n" + body[5:15] + b"\r\n" +
                      b"%x\r\n" % len(body[15:]) + body[15:] + b"\r\n" +
                      b"0;last\r\nX-Trailer: t\r\nY: u\r\n\r\n" +
                      request + ping % 7 +
                      b"POST /mcp HTTP/1.1\r\nConnection: close\r\n"
                      b"Content-Type: application/json\r\n"
                      b"Transfer-Encoding: chunked\r\n\r\n" +
                      b"%x\r\n" % len(ping % 8) + ping % 8 + b"\r\n0\r\n\r\n")
        assert [int(i) for i in re.findall(rb'"id":(\d)', wire)] == [6, 7, 8], wire
        with socket.create_connection(("127.0.0.1", ai.port), 10) as sock:
            sock.sendall(chunked.replace(b"\r\n\r\n",
                                         b"\r\nExpect: 100-continue\r\n\r\n"))
            assert sock.recv(4096) == b"HTTP/1.1 100 Continue\r\n\r\n"
            sock.sendall(b"%x\r\n" % len(ping % 9) + ping % 9 + b"\r\n0\r\n\r\n")
            assert b'"id":9' in sock.recv(4096)
        for bad, expected in (
                (b"5\r\n" + body[:5] + b"XX0\r\n\r\n", 400),  # no CRLF after data
                (b"5\n" + body[:5] + b"\r\n0\r\n\r\n", 400),   # a bare LF
                (b"g\r\n", 400),                                 # not hexadecimal
                (b";x\r\n", 400),                                # no size
                (b"5 x\r\n", 400),                               # junk after the size
                (b"5;\x01\r\n", 400),                            # a control character
                (b"1" * 17 + b"\r\n", 400),                       # overflow
                (b"0" * 2000 + b"\r\n", 400),                     # a line too long
                (b"0\r\n" + b"X: y\r\n" * 65 + b"\r\n", 431),    # trailer fields
                (b"0\r\nPOST /mcp HTTP/1.1\r\n\r\n", 400)):        # no field
            assert status_of(chunked + bad) == expected, bad
        assert status_of(chunked.replace(b"\r\n\r\n", b"\r\nContent-Length: 5"
                                                   b"\r\n\r\n")) == 400
        assert status_of(b"POST /mcp HTTP/1.1\r\nTransfer-Encoding: chunked\r\n"
                         b"Transfer-Encoding: chunked\r\n\r\n") == 400
        assert status_of(b"POST /mcp HTTP/1.1\r\n"
                         b"Transfer-Encoding: gzip, chunked\r\n\r\n") == 501
        assert status_of(b"POST /mcp HTTP/1.0\r\n"
                         b"Transfer-Encoding: chunked\r\n\r\n0\r\n\r\n") == 400
        assert status_of(b"POST /mcp HTTP/1.1\r\nMcp-Method: a\r\nMcp-Method: b\r\n"
                         b"Content-Length: 2\r\n\r\n{}") == 400
        # A list such as Accept may come in several lines (RFC 9110 5.3); its
        # first stands for it.
        call = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                           "params": {"name": "ask_ai",
                                      "arguments": {"prompt": "Q"}}}).encode()
        for first, second, sse in ((b"application/json", b"text/event-stream", False),
                                   (b"text/event-stream", b"application/json", True)):
            reply = ai.raw(b"POST /mcp HTTP/1.1\r\nContent-Type: application/json\r\n"
                           b"Accept: " + first + b"\r\nAccept: " + second + b"\r\n"
                           b"Connection: close\r\n"
                           b"Content-Length: %d\r\n\r\n" % len(call) + call)
            assert reply.startswith(b"HTTP/1.1 200 "), reply
            assert (b"Content-Type: text/event-stream" in reply) == sse, reply
            assert b'"text":"A"' in reply, reply
        assert status_of(b"POST /mcp HTTP/1.1\r\nContent-Type: application/json\r\n"
                         b"Content-Type: application/json\r\n"
                         b"Content-Length: 2\r\n\r\n{}") == 400
        reply = ai.raw(b"POST /mcp HTTP/1.0\r\nContent-Type: application/json\r\n" +
                       length + b"\r\n" + ping % 5)
        assert b"Connection: close" in reply and b'"id":5' in reply

    with Askai(binary, "-u", up.url(), env={"ASKAI_TOKEN": "s3cret"}) as ai:
        status, headers, _ = ai.rpc("ping")
        assert status == 401 and headers["www-authenticate"] == "Bearer"
        assert ai.rpc("ping", headers={"Authorization": "Bearer nope"})[0] == 401
        assert ai.rpc("ping", headers={"Authorization": "Bearer s3cret"})[0] == 200
        status, headers, _ = ai.rpc("ping", headers={"Origin": "http://localhost"})
        assert status == 401 and \
            headers["access-control-allow-origin"] == "http://localhost"
        conn = ai.connect()
        conn.request("OPTIONS", "/mcp", headers={"Origin": "http://localhost"})
        assert conn.getresponse().status == 204
        conn.close()

    print("HTTP framing, CORS, Origin and token checks: PASS")


def rpc_request(rpc_id, params=None, method="ping", accept_sse=False, headers=None):
    msg = {"jsonrpc": "2.0", "id": rpc_id, "method": method}
    if params is not None:
        msg["params"] = params
    body = json.dumps(msg).encode()
    fields = {"Content-Type": "application/json", "Content-Length": len(body)}
    if accept_sse:
        fields["Accept"] = "application/json, text/event-stream"
    fields.update(headers or {})
    return (b"POST /mcp HTTP/1.1\r\n" +
            b"".join(b"%s: %s\r\n" % (k.encode(), str(v).encode())
                     for k, v in fields.items()) + b"\r\n" + body)


def session_call(i, rpc_id=1, prompt="Q", **headers):
    """A legacy tool call of session @p i's own."""
    return rpc_request(rpc_id, {"name": "ask_ai", "arguments": {"prompt": prompt}},
                       "tools/call", headers={"Mcp-Session-Id": f"{i:032x}",
                                              "MCP-Protocol-Version": LEGACY,
                                              **headers})


def held_ping(port, rpc_id=0):
    """A keep-alive connection that has been answered once."""
    sock = socket.create_connection(("127.0.0.1", port), 10)
    sock.sendall(rpc_request(rpc_id))
    assert Replies(sock).next()[1]["id"] == rpc_id
    return sock


def closed_by_peer(sock, timeout=2):
    sock.settimeout(timeout)
    try:
        return sock.recv(1) == b""
    except ConnectionResetError:
        return True
    except TimeoutError:
        return False


def check_connections(binary, up):
    with Askai(binary, "-u", up.url()) as ai:
        # More keep-alives than the old 64-connection cap; none locks
        # a newcomer out.
        held = [held_ping(ai.port, i) for i in range(100)]
        assert ai.rpc("ping")[0] == 200
        held[0].sendall(rpc_request(7))
        assert Replies(held[0]).next()[1]["id"] == 7
        for sock in held:
            sock.close()

        # A deep pipeline is answered in order, while another client
        # still gets through.
        count = 3000
        wire = b"".join(rpc_request(i) for i in range(count))
        with socket.create_connection(("127.0.0.1", ai.port), 10) as sock:
            sender = threading.Thread(target=sock.sendall, args=(wire,))
            sender.start()
            assert ai.rpc("ping", rpc_id=99)[2]["id"] == 99
            replies = Replies(sock)
            ids = [replies.next()[1]["id"] for _ in range(count)]
            sender.join(10)
            assert ids == list(range(count)), ids[:10]

        # A head trickled byte by byte, then a large body followed by a
        # pipelined request in the same write.
        with socket.create_connection(("127.0.0.1", ai.port), 10) as sock:
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            for byte in rpc_request(1):
                sock.send(bytes((byte,)))
                time.sleep(0.0005)
            replies = Replies(sock)
            assert replies.next() == (200, {"jsonrpc": "2.0", "id": 1,
                                            "result": {}})
            sock.sendall(rpc_request(2, {"pad": "x" * 300_000}) +
                         rpc_request(3))
            assert [replies.next()[1]["id"] for _ in range(2)] == [2, 3]

        # A pipelining client that does not read: its replies back up until
        # a send would block, others are served meanwhile, and its replies
        # follow in order once it reads.
        count = 8000
        wire = b"".join(rpc_request(i, method="tools/list") for i in range(count))
        with socket.socket() as sock:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
            sock.settimeout(10)
            sock.connect(("127.0.0.1", ai.port))
            sender = threading.Thread(target=sock.sendall, args=(wire,))
            sender.start()
            time.sleep(0.3)
            assert ai.rpc("ping", rpc_id=99)[2]["id"] == 99
            replies = Replies(sock)
            ids = [replies.next()[1]["id"] for _ in range(count)]
            sender.join(10)
            assert ids == list(range(count)), ids[:10]

    # Out of descriptors, the least recently active idle connection makes
    # room; with none idle, the spare descriptor turns the newcomer away.
    with Askai(binary, "-u", up.url(), loops=1) as ai:
        fds = f"/proc/{ai.proc.pid}/fd"
        used = {int(fd) for fd in os.listdir(fds)}
        room = [fd for fd in range(max(used) + 9) if fd not in used][:8]
        resource.prlimit(ai.proc.pid, resource.RLIMIT_NOFILE, (room[-1] + 1,) * 2)

        def settle():
            for _ in range(200):
                if len(os.listdir(fds)) == len(used):
                    return
                time.sleep(0.01)

        held = [held_ping(ai.port, i) for i in range(12)]
        assert all(closed_by_peer(sock) for sock in held[:4])
        for i, sock in enumerate(held[4:]):
            sock.sendall(rpc_request(i))
            assert Replies(sock).next()[1]["id"] == i
        for sock in held:
            sock.close()
        settle()

        loris = []
        for _ in range(8):
            loris.append(socket.create_connection(("127.0.0.1", ai.port), 10))
            loris[-1].sendall(b"POST /mcp HTTP/1.1\r\n")
        with socket.create_connection(("127.0.0.1", ai.port), 10) as sock:
            assert closed_by_peer(sock), "a client beyond the limit was kept"
        assert not any(closed_by_peer(sock, 0.1) for sock in loris)
        for sock in loris:
            sock.close()
        settle()
        assert ai.rpc("ping")[0] == 200

    # Clients may hold all but 64 descriptors and the client loop's two, 128
    # here, a tool call counting one more for its upstream connection.  New
    # clients and calls beyond that make room by closing the least recently
    # active idle connection, so idle clients cannot leave tool calls without
    # a descriptor.  Each call is a session's, as one principal may have but
    # eight queued.
    up.reply = lambda request: (200, ANSWERS["openai"], 0.5)
    with Askai(binary, "-u", up.url(), "--slots", "64", nofile=194, loops=1) as ai:
        held = [held_ping(ai.port, i) for i in range(128)]
        held.append(held_ping(ai.port, 128))
        assert closed_by_peer(held[0])
        callers = held[-60:]
        for i, sock in enumerate(callers):
            sock.sendall(session_call(i))
        for sock in callers:
            _, body = Replies(sock).next()
            assert text_of(body["result"]) == "A", body
        assert all(closed_by_peer(sock) for sock in held[1:61])
        assert not any(closed_by_peer(sock, 0.1) for sock in held[61:69])
        assert ai.rpc("ping")[0] == 200
        for sock in held:
            sock.close()
    up.reply = lambda request: (200, ANSWERS["openai"])

    print("connections: no cap, pipelining, slow readers, fd pressure: PASS")


def check_cancellation(binary, up):
    def slow(request):
        return 200, ANSWERS["openai"], 1.0 if "SLOW" in request["raw"].decode() else 0

    # With two loops, home places a connection that comes while a call waits
    # on the loop that has none, so each notification reaches the call's job
    # from the other loop, through home's registry.
    with Askai(binary, "-u", up.url(), loops=2) as ai:
        def served(method):
            """How many requests of @p method each loop has served."""
            return {k: v for k, v in ai.metrics().items()
                    if k.startswith("askai_request_seconds_count{") and
                    f'method="{method}"' in k}

        def one_loop_more(before, after):
            more = [k.split('"')[1] for k in after if after[k] > before[k]]
            assert len(more) == 1, (before, after)
            return more[0]

        _, headers, _ = ai.rpc("initialize", {"protocolVersion": LEGACY})
        session = {"Mcp-Session-Id": headers["mcp-session-id"],
                   "MCP-Protocol-Version": LEGACY}
        up.reply = slow

        def call(results, rpc_id=42):
            results.append(ai.rpc("tools/call", {"name": "ask_ai", "arguments": {
                "prompt": "SLOW"}}, rpc_id=rpc_id, headers=session))

        for other in ({"Mcp-Session-Id": "f" * 32}, {}):
            up.arrived.clear()
            up.cancelled.clear()
            results = []
            thread = threading.Thread(target=call, args=(results,))
            thread.start()
            assert up.arrived.wait(5)
            status, _, _ = ai.rpc("notifications/cancelled", {"requestId": 42},
                                  rpc_id=None, headers=other)
            assert status == 202
            thread.join(5)
            assert text_of(results[0][2]["result"]) == "A", \
                "another session must not cancel the call"
            assert not up.cancelled.is_set()

        # A legacy client may cancel by notification.  On an event stream
        # the stream then ends without an event; a JSON POST gets a 202.
        for accept, expected in (("application/json, text/event-stream", 200),
                                 ("application/json", 202)):
            up.arrived.clear()
            up.cancelled.clear()
            results = []
            session["Accept"] = accept
            calls, notes = served("tools/call"), served("notification")
            thread = threading.Thread(target=call, args=(results,))
            thread.start()
            assert up.arrived.wait(5)
            status, _, _ = ai.rpc("notifications/cancelled",
                                  {"requestId": 42, "reason": "stop"},
                                  rpc_id=None, headers=session)
            assert status == 202
            thread.join(5)
            status, headers, body = results[0]
            assert (status, body) == (expected, None), results
            assert (headers.get("content-type") == "text/event-stream") == \
                (status == 200)
            assert up.cancelled.wait(2), "upstream request was not cancelled"
            assert one_loop_more(calls, served("tools/call")) != \
                one_loop_more(notes, served("notification")), \
                "the call and its cancellation came to one loop"
        del session["Accept"]

        # The longest id a request may have names a call too.
        up.arrived.clear()
        up.cancelled.clear()
        results = []
        thread = threading.Thread(target=call, args=(results, "i" * 1024))
        thread.start()
        assert up.arrived.wait(5)
        assert ai.rpc("notifications/cancelled", {"requestId": "i" * 1024},
                      rpc_id=None, headers=session)[0] == 202
        thread.join(5)
        assert (results[0][0], results[0][2]) == (200, None), results
        assert up.cancelled.wait(2), "upstream request was not cancelled"

        # Closing an event stream cancels the call in every era.
        for headers in ({"MCP-Protocol-Version": LEGACY}, {}):
            up.arrived.clear()
            up.cancelled.clear()
            with socket.create_connection(("127.0.0.1", ai.port), 10) as sock:
                sock.sendall(rpc_request(5, {"name": "ask_ai", "arguments": {
                    "prompt": "SLOW"}}, "tools/call", accept_sse=True,
                    headers=headers))
                head = sock.recv(4096)
                assert head.startswith(b"HTTP/1.1 200 OK\r\n") and \
                    b"Content-Type: text/event-stream\r\n" in head, head
                assert up.arrived.wait(5)
            assert up.cancelled.wait(2), "closing the stream did not cancel"
        up.arrived.clear()
        up.cancelled.clear()
        body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "tools/call",
                           "params": {"name": "ask_ai",
                                      "arguments": {"prompt": "SLOW"}}}).encode()
        with socket.create_connection(("127.0.0.1", ai.port), 10) as sock:
            sock.sendall(b"POST /mcp HTTP/1.1\r\nContent-Type: application/json\r\n"
                         b"Content-Length: %d\r\n\r\n" % len(body) + body)
            assert up.arrived.wait(5)
        assert up.cancelled.wait(2), "client hang-up did not cancel upstream"
        assert ai.rpc("ping")[0] == 200

        # Shutting down only the sending side cancels the call too, even
        # when the shutdown arrives with the request.
        up.arrived.clear()
        up.cancelled.clear()
        with socket.create_connection(("127.0.0.1", ai.port), 10) as sock:
            ai.proc.send_signal(signal.SIGSTOP)
            try:
                sock.sendall(b"POST /mcp HTTP/1.1\r\nContent-Type: application/json\r\n"
                             b"Content-Length: %d\r\n\r\n" % len(body) + body)
                sock.shutdown(socket.SHUT_WR)
                time.sleep(0.1)
            finally:
                ai.proc.send_signal(signal.SIGCONT)
            assert closed_by_peer(sock, 5), "a half-closed call was answered"
        assert not up.arrived.is_set() or up.cancelled.wait(2)
        assert ai.rpc("ping")[0] == 200
    up.reply = lambda request: (200, ANSWERS["openai"])

    print("cancellation by notification, from another loop too, hang-up and "
          "half-close: PASS")


def check_limits(binary, up):
    def connect(ai):
        sock = socket.create_connection(("127.0.0.1", ai.port), 10)
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        return sock

    def status_after(sock, timeout=5):
        """The status of the next reply, and how long it took to come."""
        start = time.monotonic()
        sock.settimeout(timeout)
        reply = Replies(sock).next()
        return (reply[0] if reply else None), time.monotonic() - start

    def until_closed(sock, timeout=5):
        start = time.monotonic()
        assert closed_by_peer(sock, timeout), "the connection stayed open"
        return time.monotonic() - start

    def ping_with(value):
        return json.dumps({"jsonrpc": "2.0", "id": 1, "method": "ping",
                           "params": {"a": value}}).encode()

    def post(body, length=None):
        return (b"POST /mcp HTTP/1.1\r\nContent-Type: application/json\r\n"
                b"Content-Length: %d\r\n\r\n" % (len(body) if length is None
                                                  else length) + body)

    with Askai(binary, "-u", up.url(), "--head-timeout", "1", "--idle-timeout", "1",
               "--max-fields", "4", "--max-head", "1024", "--max-body", "1000",
               "--min-body-rate", "1000", "--body-grace", "1", "--max-depth", "4",
               "--max-values", "20") as ai:
        # Deadlines count from the first byte; trickling does not move them.
        with connect(ai) as sock:
            sock.sendall(b"POST /mcp HTTP/1.1\r\n")
            for _ in range(3):
                time.sleep(0.3)
                sock.sendall(b"X")
            status, waited = status_after(sock)
            assert status == 408 and waited < 0.6, (status, waited)
            until_closed(sock)
        with connect(ai) as sock:
            sock.sendall(post(b"{", 900))
            status, waited = status_after(sock)
            assert status == 408 and 0.5 < waited < 2, (status, waited)
        # A body that keeps to the rate is served; then the idle timeout
        # closes the connection.
        parts = (b'{"jsonrpc":"2.0",', b'"id":1,', b'"method":"ping"}')
        with connect(ai) as sock:
            sock.sendall(post(parts[0], sum(map(len, parts))))
            for part in parts[1:]:
                time.sleep(0.4)
                sock.sendall(part)
            assert status_after(sock)[0] == 200
            assert 0.5 < until_closed(sock) < 2.5, "idle timeout"

        def status_of(wire):
            with connect(ai) as sock:
                sock.sendall(wire)
                return status_after(sock)[0]

        fields = b"".join(b"X%d: y\r\n" % i for i in range(4))
        assert status_of(b"POST /mcp HTTP/1.1\r\n" + fields + b"\r\n") != 431
        assert status_of(b"POST /mcp HTTP/1.1\r\nY: z\r\n" + fields + b"\r\n") == 431
        assert status_of(b"POST /mcp HTTP/1.1\r\nX: " + b"a" * 1100 + b"\r\n\r\n") == 431
        assert status_of(post(ping_with([[1]]))) == 200
        assert status_of(post(ping_with([[[1]]]))) == 400
        # Brackets and quotes in strings do not count, however the body is
        # split up on its way.
        with connect(ai) as sock:
            wire = post(ping_with('[{"\\' * 50 + "]"))
            for i in range(0, len(wire), 5):
                sock.send(wire[i:i + 5])
                time.sleep(0.001)
            assert status_after(sock)[0] == 200
        # Keys count as values: 11 come before the array's elements.
        assert status_of(post(ping_with(list(range(9))))) == 200
        assert status_of(post(ping_with(list(range(10))))) == 400
        # So is one that its chunks take over the limit.
        assert status_of(b"POST /mcp HTTP/1.1\r\nContent-Type: application/json\r\n"
                         b"Transfer-Encoding: chunked\r\n\r\n" +
                         b"200\r\n" + b" " * 512 + b"\r\n1f5\r\n") == 413
        # A body over the limit is refused.  The connection lingers, so a
        # client that sends the body all the same is not reset.
        with connect(ai) as sock:
            sock.sendall(post(b"", 200_000))
            assert status_after(sock)[0] == 413
            for _ in range(4):
                time.sleep(0.05)
                sock.sendall(b" " * 50_000)
            assert sock.recv(1) == b""

    def error_message(body):
        with Askai(binary, "-u", up.url()) as ai, connect(ai) as sock:
            sock.sendall(post(body))
            status, reply = Replies(sock).next()
            return status, reply.get("error", {}).get("message")

    # By default 32 containers may be open: the message and params, then 30.
    assert error_message(ping_with(json.loads("[" * 30 + "]" * 30))) == (200, None)
    assert error_message(ping_with(json.loads("[" * 31 + "]" * 31))) == \
        (400, "Request JSON is nested too deeply")

    # Bodies held at once share a budget, of which one client address may
    # hold its own share; clients share a cap per address.
    def connect_from(ai, addr):
        sock = socket.socket()
        sock.bind((addr, 0))
        sock.connect(("127.0.0.1", ai.port))
        return sock

    with Askai(binary, "-u", up.url(), "--body-budget", "3000", "--max-body", "1500",
               "--client-budget", "2000", "--max-client-conns", "3") as ai:
        with connect_from(ai, "127.0.0.2") as one, connect_from(ai, "127.0.0.3") as two:
            one.sendall(post(b"{", 1500))
            time.sleep(0.2)
            with connect_from(ai, "127.0.0.2") as sock:
                sock.sendall(post(b" " * 1000))
                assert status_after(sock)[0] == 503, "an address took over its share"
            with connect(ai) as sock:
                sock.sendall(post(b" " * 1000))
                assert status_after(sock)[0] == 400, "another address was refused"
            two.sendall(post(b"{", 1500))
            time.sleep(0.2)
            with connect(ai) as sock:
                sock.sendall(post(b" " * 1000))
                assert status_after(sock)[0] == 503
        time.sleep(0.2)
        assert ai.post(b" " * 1000)[0] == 400

        held = [held_ping(ai.port) for _ in range(3)]
        with connect(ai) as sock:
            assert status_after(sock)[0] == 503, "a fourth connection was served"
            until_closed(sock)
        held.pop().close()
        time.sleep(0.2)
        assert ai.rpc("ping")[0] == 200
        for sock in held:
            sock.close()

    # A client without the token is refused before its body, so it holds
    # none of the budget.
    with Askai(binary, "-u", up.url(), "--body-budget", "2000", "--max-body", "1500",
               env={"ASKAI_TOKEN": "s3cret"}) as ai, connect(ai) as sock:
        sock.sendall(post(b"{", 1500))
        status, waited = status_after(sock)
        assert status == 401 and waited < 1, (status, waited)
        auth = {"Authorization": "Bearer s3cret"}
        assert ai.post(b" " * 1000, headers=auth)[0] == 400

    # An object may have 1024 members, or --max-members: json-c rehashes
    # all of an object's members at once as it grows.
    def members(n):
        return json.dumps({"jsonrpc": "2.0", "id": 1, "method": "ping", "params": {
            "o": {f"k{i}": 0 for i in range(n)}}}).encode()
    with Askai(binary, "-u", up.url(), "--max-values", "5000") as ai:
        assert ai.post(members(1024))[0] == 200
        status, _, body = ai.post(members(1025))
        assert status == 400 and "too many members" in body["error"]["message"]
    with Askai(binary, "-u", up.url(), "--max-values", "5000",
               "--max-members", "2000") as ai:
        assert ai.post(members(2000))[0] == 200
        assert ai.post(members(2001))[0] == 400

    # The default value limit does not grow with the body limit, as parsing
    # and freeing values costs the loop more than bytes do.  Ids are short.
    with Askai(binary, "-u", up.url(), "--max-body", "16M") as ai:
        assert ai.post(ping_with([{}] * 1013))[0] == 200
        status, _, body = ai.post(ping_with([{}] * 1014))
        assert status == 400 and "too many values" in body["error"]["message"]
        assert ai.rpc("ping", rpc_id="i" * 1024)[0] == 200
        status, _, body = ai.rpc("ping", rpc_id="i" * 1025)
        assert status == 400 and body["error"]["message"] == "Invalid request id"
        # json-c converts a number whole, so one may take 1 KiB at most.
        number = b'{"jsonrpc":"2.0","id":1,"method":"ping","params":{"n":0.%s}}'
        assert ai.post(number % (b"1" * 1022))[0] == 200
        status, _, body = ai.post(number % (b"1" * 1023))
        assert status == 400 and "too long a number" in body["error"]["message"]

    # A client that never reads is closed once its output stalls, or once
    # it would exceed the output limit.
    wire = b"".join(rpc_request(i, method="tools/list") for i in range(8000))
    for args in (("--send-timeout", "1"),
                 ("--output-pause", "64K", "--max-output", "64K")):
        with Askai(binary, "-u", up.url(), *args) as ai:
            with socket.socket() as sock:
                sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
                sock.connect(("127.0.0.1", ai.port))
                sender = threading.Thread(target=sock.sendall, args=(wire,),
                                          daemon=True)
                sender.start()
                time.sleep(2.5)
                # askai has closed it, so reading ends at EOF or a reset
                # rather than timing out.
                sock.settimeout(5)
                try:
                    while sock.recv(65536):
                        pass
                except ConnectionResetError:
                    pass
            assert ai.rpc("ping")[0] == 200

    # A client that half-closes while its replies wait to be sent leaves
    # askai idle, not spinning on the input that ended.
    with Askai(binary, "-u", up.url(), "--output-pause", "64M",
               "--max-output", "64M") as ai:
        def cpu():
            with open(f"/proc/{ai.proc.pid}/stat") as f:
                ticks = sum(map(int, f.read().rsplit(")", 1)[1].split()[11:13]))
            return ticks / os.sysconf("SC_CLK_TCK")

        count = 8000
        with socket.socket() as sock:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
            sock.connect(("127.0.0.1", ai.port))
            sock.sendall(b"".join(rpc_request(i, method="tools/list")
                                  for i in range(count)))
            sock.shutdown(socket.SHUT_WR)
            time.sleep(0.5)
            used = cpu()
            time.sleep(1)
            assert cpu() - used < 0.2, "askai spins"
            replies = Replies(sock)
            assert [replies.next()[1]["id"] for _ in range(count)] == \
                list(range(count))
            assert replies.next() is None

    # SIGUSR1 prints each loop's statistics since the previous one, in lines
    # that name it: each client loop's, and home's.
    with Askai(binary, "-u", up.url()) as ai:
        assert ai.rpc("ping")[0] == 200
        ai.proc.send_signal(signal.SIGUSR1)
        loops = ai.loops()
        assert len(loops) == 5, loops
        for _ in range(100):
            if sum(re.match(r"askai: stats \w+ work ", line) is not None
                   for line in ai.log) == len(loops):
                break
            time.sleep(0.01)
        stats = [line.split()[2:4] for line in ai.log if line.startswith("askai: stats ")]
        names = ["turn", "interactive", "bulk", "timer_late", "work"]
        for loop in loops:
            assert [s[1] for s in stats if s[0] == loop] == names, (loop, ai.log)
        assert re.search(r"^askai: stats loop\d+ turn n=[1-9]\d* ", "".join(ai.log),
                         re.M)
        # The most work of each unit a step did, against its bound, on the
        # loop that served the ping.
        line = next(line for line in ai.log
                    if re.match(r"askai: stats loop\d+ work .* reqs=1/", line))
        units = {k: tuple(map(int, v.split("/")))
                 for k, v in re.findall(r" (\w+)=(\d+/\d+)", line)}
        assert set(units) >= {"events", "timers", "reqs", "recv", "send", "body",
                              "parse", "values", "escape", "copy", "accepts"}, units
        assert units["reqs"][0] > 0 and units["parse"][0] > 0, units
        assert all(most <= bound for most, bound in units.values()), units

    # Work that cannot be split counts in full where it happens, so the line
    # shows a step that went past a bound: here json-c copying a string of
    # 2 MiB whole, which a raised body limit lets in.
    with Askai(binary, "-u", up.url(), "--max-body", "4M") as ai:
        work(ai)
        assert ai.rpc("ping", {"s": "x" * (2 << 20)})[0] == 200
        units, _ = work(ai)
        assert units["copy"][0] >= 2 << 20 > units["copy"][1], units
        assert units["parse"][0] <= units["parse"][1], units

    print("limits, deadlines, lingering and loop statistics: PASS")


def check_loops(binary, up):
    """Home accepts the clients and places each on the lighter of two client
    loops (design section 3), and the limits that hold for the process hold
    across the loops: connections per client address, the bodies held, for
    all and for an address, and the descriptors, which run short no matter
    which loops' clients hold them."""
    def connect_from(ai, addr):
        sock = socket.socket()
        sock.bind((addr, 0))
        sock.connect(("127.0.0.1", ai.port))
        return sock

    def ping_from(ai, addr, rpc_id=0):
        sock = connect_from(ai, addr)
        sock.sendall(rpc_request(rpc_id))
        return sock, Replies(sock).next()[0]

    def per_loop(ai):
        """Each client loop's connections, the metrics' own among them."""
        return [v for k, v in sorted(ai.metrics().items())
                if k.startswith("askai_connections{")]

    def post(body, length=None):
        return (b"POST /mcp HTTP/1.1\r\nContent-Type: application/json\r\n"
                b"Content-Length: %d\r\n\r\n" % (len(body) if length is None
                                                  else length) + body)

    # Placement spreads the connections over the loops: the lighter of two
    # leaves them a few apart, where random placement would pass one time in
    # twelve.
    with Askai(binary, "-u", up.url()) as ai:
        held = [held_ping(ai.port, i) for i in range(200)]
        conns = per_loop(ai)
        assert len(conns) == 4 and sum(conns) == 201, conns
        assert max(conns) - min(conns) <= 6, conns
        for sock in held:
            sock.close()
        values = ai.metrics()
        assert total(values, "askai_connections_placed_total") >= 202, values

    # An address's connections, spread over the loops, are capped all the
    # same, while another address is served.
    with Askai(binary, "-u", up.url(), "--max-client-conns", "6") as ai:
        held = []
        for i in range(6):
            sock, status = ping_from(ai, "127.0.0.2", i)
            assert status == 200
            held.append(sock)
        assert sum(n > 0 for n in per_loop(ai)) >= 3
        sock, status = ping_from(ai, "127.0.0.2")
        assert status == 503, "a seventh connection was served"
        sock.close()
        sock, status = ping_from(ai, "127.0.0.3")
        assert status == 200
        sock.close()
        held.pop().close()
        time.sleep(0.2)
        sock, status = ping_from(ai, "127.0.0.2")
        assert status == 200
        sock.close()
        for sock in held:
            sock.close()

    # Bodies held on several loops share a budget, of which one address may
    # hold its share: eight connections of one address, spread over the
    # loops, each hold a body's length, a tenth of the address's share,
    # 1600 bytes in all.
    with Askai(binary, "-u", up.url(), "--body-budget", "3000", "--max-body", "1500",
               "--client-budget", "2000") as ai:
        held = [connect_from(ai, "127.0.0.2") for _ in range(8)]
        for sock in held:
            sock.sendall(post(b"{", 200))
        time.sleep(0.2)
        assert sum(n > 0 for n in per_loop(ai)) >= 3
        with connect_from(ai, "127.0.0.2") as sock:
            sock.sendall(post(b" " * 500))
            assert Replies(sock).next()[0] == 503, "an address took over its share"
        with connect_from(ai, "127.0.0.3") as sock:
            sock.sendall(post(b" " * 500))
            assert Replies(sock).next()[0] == 400, "another address was refused"
        with connect_from(ai, "127.0.0.3") as other:
            other.sendall(post(b"{", 1000))
            time.sleep(0.2)
            with connect_from(ai, "127.0.0.4") as sock:
                sock.sendall(post(b" " * 500))
                assert Replies(sock).next()[0] == 503, "the bodies took over the budget"
        for sock in held:
            sock.close()
        time.sleep(0.2)
        assert ai.post(b" " * 1000)[0] == 400

    # Clients and jobs hold all but 64 descriptors and the client loops' own,
    # two a loop, 128 here: past that, the loops close idle connections, the
    # least recently active first, as near as the loops can tell home.
    up.reply = lambda request: (200, ANSWERS["openai"], 0.5)
    call = rpc_request(1, {"name": "ask_ai", "arguments": {"prompt": "Q"}},
                       "tools/call")
    with Askai(binary, "-u", up.url(), "--slots", "64", nofile=200) as ai:
        held = [held_ping(ai.port, i) for i in range(129)]
        assert closed_by_peer(held[0])
        for i, sock in enumerate(held[-60:]):
            sock.sendall(session_call(i))
        for sock in held[-60:]:
            _, body = Replies(sock).next()
            assert text_of(body["result"]) == "A", body
        kept = [i for i in range(1, 69) if not closed_by_peer(held[i], 0.1)]
        assert len(kept) == 8 and min(kept) >= 40, kept
        assert ai.rpc("ping")[0] == 200
        for sock in held:
            sock.close()

    # With 32 loops, whose own descriptors are more than the 64, clients and
    # jobs may hold half of 192, as the loops' and the 64 are over half: 115
    # idle clients and 10 tool calls close 29 idle connections, or nearly,
    # most of them the oldest, and every call gets a descriptor for its
    # upstream connection.
    with Askai(binary, "-u", up.url(), loops=32, nofile=192) as ai:
        held = [held_ping(ai.port, i) for i in range(115)]
        for sock in held[-10:]:
            sock.sendall(call)
        for sock in held[-10:]:
            _, body = Replies(sock).next()
            assert text_of(body["result"]) == "A", body
        closed = [i for i in range(105) if closed_by_peer(held[i], 0.1)]
        assert 27 <= len(closed) <= 29 and sum(closed) < 25 * len(closed), closed
        for sock in held:
            sock.close()
    up.reply = lambda request: (200, ANSWERS["openai"])

    print("loops: placement, and connections, bodies and descriptors limited "
          "across loops: PASS")


def check_large_bodies(binary, up):
    """Bodies are parsed, and client strings escaped into the upstream request,
    a slice at a time: long text with escapes and multi-byte characters, where
    any slice may end, arrives whole, in every protocol's order of fields."""
    prompt = 'Ünïcödé "quoted"\n\t\u20ac\U0001f600 \\ \x01 end. ' * 30000
    system = "s\u00e9\x02" * 20000
    for protocol in ("openai", "responses", "anthropic", "gemini"):
        path = "/v1beta/models/m:generateContent" if protocol == "gemini" else "/x"
        with Askai(binary, "--api", API[protocol], "-u", up.url(path), "-m", "m",
                   "--max-body", "4M") as ai:
            up.reply = lambda request, p=protocol: (200, ANSWERS[p])
            msg = {"jsonrpc": "2.0", "id": 1, "method": "tools/call", "params": {
                "name": "ask_ai", "arguments": {"prompt": prompt, "system": system}}}
            wire = json.dumps(msg, ensure_ascii=False).encode()
            assert len(wire) > 1 << 20
            status, _, body = ai.post(wire)
            assert status == 200 and text_of(body["result"]) == "A", body
            sent = up.last()["body"]
            texts = {
                "openai": lambda b: (b["messages"][1]["content"],
                                     b["messages"][0]["content"]),
                "responses": lambda b: (b["input"][0]["content"], b["instructions"]),
                "anthropic": lambda b: (b["messages"][0]["content"], b["system"]),
                "gemini": lambda b: (b["contents"][0]["parts"][0]["text"],
                                     b["systemInstruction"]["parts"][0]["text"]),
            }[protocol](sent)
            assert texts == (prompt, system), protocol
    up.reply = lambda request: (200, ANSWERS["openai"])

    with Askai(binary, "-u", up.url(), "-s", "SYS", "-x", '{"messages": null}') as ai:
        assert text_of(ai.ask("Q" * 100_000)) == "A"
        assert up.last()["body"] == {"stream": True}
    with Askai(binary, "-u", up.url()) as ai:
        ping = b'{"jsonrpc":"2.0","id":1,"method":"ping"}'
        assert ai.post(ping + b" " * 100_000)[0] == 200
        status, _, body = ai.post(ping + b" " * 100_000 + b"x")
        assert status == 400 and body["error"]["code"] == -32700

    print("large bodies, parsed and passed on a slice at a time: PASS")


def check_sse(binary, up):
    """Tool calls answer on an event stream when the client accepts one and
    its era has them: one id-less data: event, after ':' keep-alives while the
    upstream works.  Other requests, other clients and 2024-11-05 get JSON."""
    up.reply = lambda request: (200, ANSWERS["openai"], 2.5)
    with Askai(binary, "-u", up.url(), "--sse-keepalive", "1") as ai:
        call = {"name": "ask_ai", "arguments": {"prompt": "Q"}}
        conn = ai.connect()
        conn.request("POST", "/mcp", body=json.dumps({
            "jsonrpc": "2.0", "id": 3, "method": "tools/call", "params": call}),
            headers={"Content-Type": "application/json",
                     "Accept": "application/json, text/event-stream"})
        response = conn.getresponse()
        headers = {k.lower(): v for k, v in response.getheaders()}
        raw = response.read()
        conn.close()
        assert response.status == 200
        assert headers["content-type"] == "text/event-stream"
        assert headers["cache-control"] == "no-cache"
        assert headers["x-accel-buffering"] == "no"
        assert headers["transfer-encoding"] == "chunked"
        assert "content-length" not in headers
        events, comments = sse_events(raw)
        assert len(comments) >= 2, raw
        assert not re.search(rb"^(id|event|retry):", raw, re.M), raw
        result = json.loads(events[0])
        assert result["id"] == 3 and text_of(result["result"]) == "A"

    up.reply = lambda request: (200, ANSWERS["openai"])
    with Askai(binary, "-u", up.url()) as ai:
        call = {"name": "ask_ai", "arguments": {"prompt": "Q"}}
        status, headers, body = ai.modern("tools/call", call)
        assert headers["content-type"] == "text/event-stream"
        assert body["result"]["resultType"] == "complete"
        assert text_of(body["result"]) == "A"
        for extra in ({"Accept": "application/json"},
                      {"MCP-Protocol-Version": "2024-11-05"}):
            status, headers, body = ai.rpc("tools/call", call, headers=extra)
            assert headers["content-type"] == "application/json", extra
            assert text_of(body["result"]) == "A"
        # Errors are no event streams, nor are other methods.
        status, headers, body = ai.rpc("tools/call", {"name": "ask_ai",
                                                      "arguments": {}})
        assert headers["content-type"] == "application/json"
        assert body["result"]["isError"] is True
        assert ai.rpc("ping")[1]["content-type"] == "application/json"
        # An HTTP/1.0 client gets the stream unchunked, ended by the close.
        body = json.dumps({"jsonrpc": "2.0", "id": 4, "method": "tools/call",
                           "params": call}).encode()
        reply = ai.raw(b"POST /mcp HTTP/1.0\r\nContent-Type: application/json\r\n"
                       b"Accept: text/event-stream\r\n"
                       b"Content-Length: %d\r\n\r\n" % len(body) + body)
        head, _, rest = reply.partition(b"\r\n\r\n")
        assert b"Content-Type: text/event-stream" in head and \
            b"Connection: close" in head and b"Transfer-Encoding" not in head, head
        events, _ = sse_events(rest)
        assert json.loads(events[0])["id"] == 4

    print("event streams: headers, keep-alives, one event, JSON fallbacks: PASS")


def work(ai):
    """The most work of each unit a step did since the last call, as (most,
    bound), the client loops' the most any did, home's named "home_" and its
    unit, and how many bulk steps the client loops ran."""
    ai.log.clear()
    ai.proc.send_signal(signal.SIGUSR1)
    loops = ai.loops()
    lines = tuple(f"askai: stats {loop} work " for loop in loops)
    for _ in range(200):
        if sum(line.startswith(lines) for line in ai.log) == len(lines):
            break
        time.sleep(0.01)
    units = {}
    for prefix, loop in zip(lines, loops):
        line = next(line for line in ai.log if line.startswith(prefix))
        name = "home_" if loop == "home" else ""
        for k, v in re.findall(r" (\w+)=(\d+/\d+)", line):
            most, bound = map(int, v.split("/"))
            units[name + k] = (max(most, units.get(name + k, (0,))[0]), bound)
    steps = sum(int(re.search(r" n=(\d+)", line).group(1)) for line in ai.log
                if re.match(r"askai: stats loop\d+ bulk ", line))
    return units, steps


def check_answers(binary, up):
    """An upstream answer is parsed a slice at a time, however many values it
    holds, and its reply goes out a piece at a time as the client reads it,
    however large: a response on its own may take the pending output past
    --max-output.  A plain answer is parsed on the client loop, and reading
    its text copies it whole, which the work line shows once that passes a
    step's bound; home reads a streamed one's events a slice at a time into
    its text, which the client loop only escapes.  Home takes an answer in as
    much a step as it may receive, holding the rest back in libcurl, up to
    16 MiB."""
    call = {"name": "ask_ai", "arguments": {"prompt": "Q"}}
    for plain in (False, True):
        up.plain = plain
        for args, size in ((("--output-pause", "16K", "--max-output", "64K"),
                            100_000), ((), 1_100_000)):
            text = "x\n\"" * (size // 3)
            up.reply = lambda request, t=text: (200, {"choices": [{
                "message": {"content": t}, "finish_reason": "stop"}]})
            with Askai(binary, "-u", up.url(), *args) as ai:
                work(ai)
                for accept in ("application/json",
                               "application/json, text/event-stream"):
                    status, headers, body = ai.rpc("tools/call", call,
                                                   headers={"Accept": accept})
                    assert status == 200 and text_of(body["result"]) == text, accept
                    assert headers["content-type"] == accept.split(",")[-1].strip()
                with socket.socket() as sock:
                    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
                    sock.connect(("127.0.0.1", ai.port))
                    sock.sendall(rpc_request(5, call, method="tools/call"))
                    time.sleep(0.5)
                    status, body = Replies(sock).next()
                    assert status == 200 and text_of(body["result"]) == text
                units, _ = work(ai)
                assert units["send"][0] <= units["send"][1], units
                assert (units["copy"][0] > units["copy"][1]) == \
                    (plain and size > 1 << 20), units
                assert all(units["home_" + unit][0] <= units["home_" + unit][1]
                           for unit in ("recv", "parse", "values", "copy")), units
                assert units["home_recv"][0] > 0, units
                if not plain:
                    assert units["home_parse"][0] > 0, units

    values = 200_000
    dense = {"choices": [{"message": {"content": "A"}, "finish_reason": "stop",
                          "logprobs": {"content": [0] * values}}]}
    event = {"choices": [{"index": 0, "delta": {"content": "A"},
                          "logprobs": {"content": [0] * values}}]}
    for plain, reply in ((True, (200, dense)),
                         (False, (200, Stream(wire=sse_wire([(None, event),
                                                             (None, "[DONE]")]))))):
        up.plain = plain
        up.reply = lambda request, r=reply: r
        with Askai(binary, "-u", up.url()) as ai:
            work(ai)
            assert text_of(ai.ask()) == "A"
            units, bulk = work(ai)
            assert all(most <= bound for most, bound in units.values()), units
            assert not plain or bulk >= values // units["values"][1], (bulk, units)

            # Past 16 MiB an answer is cut off, a share a step at a time too.
            up.reply = lambda request: (200, {"choices": [{"message": {
                "content": "x" * (17 << 20)}, "finish_reason": "stop"}]})
            result = ai.ask()
            assert result["isError"] and \
                text_of(result) == "Upstream response exceeds 16 MiB.", text_of(result)
            units, _ = work(ai)
            assert 0 < units["home_recv"][0] <= units["home_recv"][1], units
    up.plain = False
    up.reply = lambda request: (200, ANSWERS["openai"])

    print("answers parsed, and replies sent, a slice at a time: PASS")


def check_dense(binary, up):
    """With the value limit raised, a body of many small values is parsed, and
    freed, a few hundred values a step, and other clients are served
    meanwhile; one that turns out bad, or whose client goes, is dropped the
    same way."""
    values = 300_000
    dense = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "ping",
                        "params": {"a": [0] * values,
                                   "o": [{f"k{i}": [] for i in range(1000)}] * 20}},
                       separators=(",", ":")).encode()
    level = tempfile.NamedTemporaryFile("w", prefix="askai-pressure-")
    with Askai(binary, "-u", up.url(), "--max-values", "1000000", "--max-body", "4M",
               "--test-pressure", level.name) as ai:
        with open(f"/proc/{ai.proc.pid}/status") as f:
            start_rss = int(re.search(r"VmRSS:\s+(\d+)", f.read()).group(1))
        work(ai)
        stop = threading.Event()
        pings = []

        def probe():
            conn = ai.connect()
            while not stop.is_set():
                start = time.monotonic()
                assert ai.rpc("ping", conn=conn)[0] == 200
                pings.append(time.monotonic() - start)
            conn.close()

        thread = threading.Thread(target=probe)
        thread.start()
        assert ai.post(dense)[2] == {"jsonrpc": "2.0", "id": 1, "result": {}}
        time.sleep(0.2)
        stop.set()
        thread.join(10)
        units, bulk = work(ai)
        assert units["parse"][0] <= units["parse"][1] and \
            units["values"][0] <= units["values"][1], units
        assert bulk >= 2 * values // units["values"][1], (bulk, units)
        assert len(pings) > 10 and max(pings) < 1, (len(pings), max(pings))

        # A dense body found bad at its end, and one whose client hangs up
        # while it is parsed, are freed a slice at a time too.
        status, _, body = ai.post(dense[:-1] + b"x")
        assert status == 400 and body["error"]["code"] == -32700
        with socket.create_connection(("127.0.0.1", ai.port), 10) as sock:
            sock.sendall(b"POST /mcp HTTP/1.1\r\nContent-Type: application/json\r\n"
                         b"Content-Length: %d\r\n\r\n" % len(dense) + dense)
            time.sleep(0.01)
        time.sleep(0.5)
        units, _ = work(ai)
        assert units["values"][0] <= units["values"][1], units
        assert ai.rpc("ping")[0] == 200

        # The memory those took stays for reuse, without memory pressure,
        # and goes back under it, which the test aid induces; ASan's and
        # TSan's allocators keep it.
        def rss():
            with open(f"/proc/{ai.proc.pid}/status") as f:
                return int(re.search(r"VmRSS:\s+(\d+)", f.read()).group(1))
        with open(f"/proc/{ai.proc.pid}/exe", "rb") as f:
            exe = f.read()
            sanitized = b"__asan_init" in exe or b"__tsan_init" in exe
        kept = rss()
        time.sleep(1.5)
        if memory_calm():
            values = ai.metrics()
            assert total(values, "askai_memory_trims_total") == 0, values
            assert sanitized or rss() > max(kept - 2048, start_rss + 16384), \
                (start_rss, kept, rss())
        set_pressure(level, 2)
        for _ in range(0 if sanitized else 80):
            if rss() < start_rss + 16384:
                break
            time.sleep(0.1)
        assert sanitized or rss() < start_rss + 16384, (start_rss, kept, rss())
        wait_for(lambda: total(ai.metrics(), "askai_memory_trims_total") >= 1)
        set_pressure(level, 0)

        # Its JSON's limits hold however a chunked body is cut up.
        deep = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "ping",
                           "params": {"a": json.loads("[" * 31 + "]" * 31)}}).encode()
        wire = b"".join(b"%x\r\n%s\r\n" % (len(deep[i:i + 3]), deep[i:i + 3])
                        for i in range(0, len(deep), 3))
        with socket.create_connection(("127.0.0.1", ai.port), 10) as sock:
            sock.sendall(b"POST /mcp HTTP/1.1\r\nContent-Type: application/json\r\n"
                         b"Transfer-Encoding: chunked\r\n\r\n" + wire +
                         b"0\r\n\r\n" + rpc_request(2))
            replies = Replies(sock)
            status, body = replies.next()
            assert status == 400 and "nested too deeply" in body["error"]["message"]
            assert replies.next()[1]["id"] == 2

    print("dense bodies, parsed and freed a slice at a time: PASS")


def memory_calm():
    """Whether the machine has memory to spare, a fifth of it, far from any
    pressure askai acts on of itself."""
    with open("/proc/meminfo") as f:
        info = dict(re.findall(r"(\w+):\s+(\d+) kB", f.read()))
    return int(info["MemAvailable"]) * 5 > int(info["MemTotal"])


def set_pressure(file, level):
    """Have the test aid's file give memory pressure @p level."""
    file.seek(0)
    file.truncate()
    file.write(f"{level}\n")
    file.flush()


def wait_for(cond, seconds=5):
    """Wait up to @p seconds for @p cond() to hold, and assert that it does."""
    end = time.monotonic() + seconds
    while not cond():
        assert time.monotonic() < end, "timed out"
        time.sleep(0.05)


def check_turn_order(binary, up):
    """Work that steps queue, such as a tool call's reply, takes turns with
    work that events queue: a flood of the latter does not hold it back."""
    soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
    resource.setrlimit(resource.RLIMIT_NOFILE, (max(soft, min(hard, 4096)), hard))
    count = 600
    call = rpc_request(1, {"name": "ask_ai", "arguments": {"prompt": "Q"}},
                       "tools/call")
    up.reply = lambda request: (200, ANSWERS["openai"], 0.2)
    up.arrived.clear()
    with Askai(binary, "-u", up.url(), "--max-client-conns", "0", loops=1) as ai:
        held = [held_ping(ai.port, i) for i in range(count)]
        caller = socket.create_connection(("127.0.0.1", ai.port), 10)
        caller.sendall(call)
        assert up.arrived.wait(5)
        # While askai is stopped, the upstream answers and every client
        # sends a step's worth of pings.
        ai.proc.send_signal(signal.SIGSTOP)
        try:
            time.sleep(0.5)
            pings = b"".join(rpc_request(i) for i in range(8))
            for sock in held:
                sock.sendall(pings)
        finally:
            ai.proc.send_signal(signal.SIGCONT)
        # epoll lists sockets in the order they became readable.
        with select.epoll() as ep:
            for sock in [caller] + held:
                ep.register(sock.fileno(), select.EPOLLIN)
            time.sleep(2)
            order = [fd for fd, _ in ep.poll(0, count + 1)]
        assert len(order) == count + 1, len(order)
        assert order.index(caller.fileno()) < count // 2, \
            f"the reply came after {order.index(caller.fileno())} of {count} clients"
        assert text_of(Replies(caller).next()[1]["result"]) == "A"
        for sock in held + [caller]:
            sock.close()
    up.reply = lambda request: (200, ANSWERS["openai"])

    print("turn order: replies are not held back by a flood of requests: PASS")


def check_isolation(binary, up):
    """A socket that cannot be watched fails its own tool call, not askai."""
    source = os.path.join(os.path.dirname(os.path.abspath(__file__)), "epfail.c")
    with tempfile.TemporaryDirectory() as tmp:
        shim = os.path.join(tmp, "epfail.so")
        build = subprocess.run([os.environ.get("CC", "cc"), "-shared", "-fPIC", "-o",
                                shim, source], capture_output=True, text=True,
                               check=False)
        assert build.returncode == 0, build.stderr
        with Askai(binary, "-u", up.url(), preload=shim) as ai:
            for _ in range(3):
                result = ai.ask()
                assert result["isError"] and "cannot watch the socket" in \
                    text_of(result), result
            assert ai.rpc("ping")[0] == 200

        # A connection's own error from accept() does not hold up the next.
        with Askai(binary, "-u", up.url(), preload=shim,
                   env={"EPFAIL_ACCEPT": "1"}) as ai:
            for _ in range(4):
                start = time.monotonic()
                assert ai.rpc("ping")[0] == 200
                assert time.monotonic() - start < 1, "accepting paused"
            assert not any("accept" in line for line in ai.log), ai.log

        # What follows the loop's last turn is no turn: a slow teardown is
        # neither reported nor taken for a stuck loop.
        with Askai(binary, "-u", up.url(), preload=shim,
                   env={"EPFAIL_CLEANUP_MS": "1200"}) as ai:
            assert ai.rpc("ping")[0] == 200
        assert not any("watchdog" in line for line in ai.log), ai.log

        # A client loop that cannot start, the second of four, shuts the
        # others down, which have started, and askai exits 1.
        env = {k: v for k, v in os.environ.items() if k not in KEY_VARS}
        env.update(LD_PRELOAD=shim, EPFAIL_EPOLL="3", ASAN_OPTIONS=":".join(filter(
            None, (env.get("ASAN_OPTIONS"), "verify_asan_link_order=0"))))
        start = time.monotonic()
        run = subprocess.run([binary, "-l", "127.0.0.1:0", "-u", up.url(), "--loops",
                              "4"], env=env, stdin=subprocess.DEVNULL,
                             capture_output=True, text=True, timeout=10, check=False)
        assert run.returncode == 1 and "cannot start loop2's event loop" in \
            run.stderr, (run.returncode, run.stderr)
        assert time.monotonic() - start < 3, run.stderr

    print("failure isolation: an unwatchable upstream socket, accept errors, "
          "a slow teardown, a loop that cannot start: PASS")


def check_configuration(binary, up):
    def fails(*args, env=None, message):
        child_env = {k: v for k, v in os.environ.items() if k not in KEY_VARS}
        child_env.update(env or {})
        run = subprocess.run([binary, "-l", "127.0.0.1:0", *args], env=child_env,
                             stdin=subprocess.DEVNULL, capture_output=True,
                             text=True, timeout=5, check=False)
        assert run.returncode == 2 and message in run.stderr, (args, run.stderr)

    fails("-p", "anthropic", "-m", "m", message="set ANTHROPIC_API_KEY")
    fails("-p", "google", env={"GEMINI_API_KEY": "k"}, message="--model is required")
    fails("-u", up.url(), "-k", "UNSET_VAR", message="UNSET_VAR is not set")
    fails("-p", "cohere", message="unknown provider: cohere")
    fails("--api", "openai", message="unknown API: openai")
    fails("-m", "m", message="--provider or --url is required")
    fails("--api", "messages", "-m", "m", message="--provider or --url is required")
    fails("-p", "openai", "--api", "messages", "-m", "m", env={"OPENAI_API_KEY": "k"},
          message="openai has no messages API")
    fails("-p", "llama", "--api", "generate", message="llama has no generate API")
    fails("-u", "ftp://x", message="not an http(s) URL")
    fails("-t", "warm", message="invalid temperature")
    fails("-t", "-1", message="invalid temperature")
    fails("-n", "0", message="invalid max token count")
    fails("-x", "[1]", message="needs a JSON object")
    fails("-H", "Bad Header: x", message="invalid header line")
    fails("--tool-name", "ask ai", message="tool names")
    fails("-u", up.url(), "--upstream-tools", "--tool-name", "wait",
          message="--tool-name: askai has another tool named wait")
    fails("--max-body", "1X", message="invalid --max-body")
    fails("--max-head", "65536", message="invalid --max-head")
    fails("--max-depth", "0", message="invalid --max-depth")
    fails("--sse-keepalive", "x", message="invalid --sse-keepalive")
    fails("--max-members", "0", message="invalid --max-members")
    fails("--max-body", "2M", "--body-budget", "1M",
          message="--max-body exceeds --body-budget")
    fails("--max-body", "2M", "--client-budget", "1M",
          message="--max-body exceeds --client-budget")
    fails("--output-pause", "2M", message="--output-pause exceeds --max-output")
    fails("--max-calls", "0", message="invalid --max-calls")
    fails("--slots", "0", message="invalid --slots")
    fails("--slots", "65536", message="invalid --slots")
    fails("--chunk-timeout", "-1", message="invalid --chunk-timeout")
    for loops in ("256", "-1", "x", ""):
        fails("--loops", loops, message="invalid --loops")

    with Askai(binary, "-p", "google", "-m", "m",
               env={"GOOGLE_API_KEY": "fallback-key"}) as ai:
        _, _, body = ai.rpc("tools/list")
        assert body["result"]["tools"][0]["name"] == "ask_ai"

    with Askai(binary, "-u", up.url(), "-H", "Authorization: Bearer from-header",
               "--tool-name", "ask_twin", "--tool-description", "Ask the twin.",
               "-k", "TEST_KEY", env={"TEST_KEY": "ignored"}) as ai:
        _, _, body = ai.rpc("tools/list")
        tool = body["result"]["tools"][0]
        assert tool["name"] == "ask_twin" and tool["description"] == "Ask the twin."
        status, _, body = ai.rpc("tools/call", {"name": "ask_twin",
                                                "arguments": {"prompt": "Q"}})
        assert text_of(body["result"]) == "A"
        assert up.last()["headers"]["authorization"] == "Bearer from-header"

    # A URL that --url gives has ASKAI_API_KEY's key, if any, and never a
    # provider's usual one; its API is the provider's, else chat completions.
    for args, env, auth, path in (
            ((), {}, None, "/x"),
            ((), {"ASKAI_API_KEY": "askai-key"}, "Bearer askai-key", "/x"),
            (("-p", "openai"), {"OPENAI_API_KEY": "must-not-leak",
                                "ASKAI_API_KEY": "askai-key"}, "Bearer askai-key", "/x"),
            (("-p", "anthropic"), {"ANTHROPIC_API_KEY": "must-not-leak"}, None,
             "/v1/messages"),
            (("-p", "llama", "--api", "messages"), {"LLAMA_API_KEY": "must-not-leak"},
             None, "/v1/messages")):
        with Askai(binary, *args, "-u", up.url(path), env=env) as ai:
            up.reply = lambda request, p=path: (200, ANSWERS["anthropic" if p != "/x"
                                                             else "openai"])
            assert text_of(ai.ask()) == "A", args
            headers = up.last()["headers"]
            assert headers.get("authorization") == auth, (args, headers)
            assert "x-api-key" not in headers, (args, headers)
    up.reply = lambda request: (200, ANSWERS["openai"])

    # A configuration file: a JSON object, comments allowed, whose members are
    # the long options, which the command line overrides.
    with tempfile.TemporaryDirectory() as d:
        path = f"{d}/askai.json"

        def write(text, mode=0o600):
            with open(path, "w") as f:
                f.write(text)
            os.chmod(path, mode)

        write("""{
            // the upstream
            "url": "%s", "tool-name": "ask_file", /* and its tool */
            "tool-description": "From the file.", "man": true,
            "header": ["X-A: 1", "X-B: 2"], "temperature": 0.5,
            "extra": {"top_k": 5}, "max-tokens": 9, "test-fifo": false,
        }""" % up.url())
        with Askai(binary, "-c", path, "--tool-name", "ask_cli", "-n", "7",
                   "-H", "X-C: 3") as ai:
            tools = ai.rpc("tools/list")[2]["result"]["tools"]
            assert [(t["name"], t["description"]) for t in tools[:1]] == \
                [("ask_cli", "From the file.")] and tools[1]["name"] == "man", tools
            _, _, body = ai.rpc("tools/call", {"name": "ask_cli",
                                               "arguments": {"prompt": "Q"}})
            assert text_of(body["result"]) == "A", body
            request = up.last()
            assert [request["headers"][k] for k in ("x-a", "x-b", "x-c")] == \
                ["1", "2", "3"], request["headers"]
            assert {k: request["body"].get(k) for k in ("temperature", "top_k",
                                                         "max_tokens")} == \
                {"temperature": 0.5, "top_k": 5, "max_tokens": 7}, request["body"]

        url = '"url": "%s"' % up.url()
        for text, message in (
                ('{%s, "bogus": 1}' % url, f"{path}: unknown option: bogus"),
                ('{%s, "config": "x"}' % url, "unknown option: config"),
                ('{%s, "loops": [1]}' % url, f"{path}: invalid --loops: [1]"),
                ('{%s, "man": "yes"}' % url, "invalid --man"),
                ('{%s, "max-body": "1X"}' % url, "invalid --max-body: 1X"),
                ('{%s, "header": [["X-A: 1", "X-B: 2"]]}' % url,
                 f'{path}: invalid --header: [["X-A: 1","X-B: 2"]]'),
                ('{"url": "ftp://x"}', f"{path}: not an http(s) URL"),
                ('{\n %s\n "loops": 1\n}' % url,
                 f"{path}: 3:2: object value separator ',' expected"),
                ('[1]', f"{path}: 1:1: not one JSON object"),
                ('{%s} x' % url, "not one JSON object"),
                ('{%s, "x": "%s"}' % (url, "y" * (1 << 20)), "larger than 1 MiB")):
            write(text)
            fails("-c", path, message=message)
        write("{%s}" % url, 0o620)
        fails("-c", path, message=f"{path}: others than root and askai's user")
        write("{%s}" % url)
        os.chmod(d, 0o770)
        fails("-c", path, message="others than root and askai's user")
        os.chmod(d, 0o700)
        os.symlink(path, f"{d}/link.json")
        fails("-c", f"{d}/link.json", message="cannot open")
        os.mkfifo(f"{d}/fifo.json")
        fails("-c", f"{d}/fifo.json", message="not a regular file")
        fails("-c", f"{d}/none.json", message="No such file or directory")
        write("{")
        run = subprocess.run([binary, "-c", path, "-h"], capture_output=True,
                             text=True, timeout=5)
        assert run.returncode == 0 and "--config FILE" in run.stdout, run

    print("configuration and key handling, and the configuration file: PASS")


def check_identities(binary, up):
    """Who a client is, by the bearer token it shows or else by how it
    connects, and its role's view and no more: the tools it lists and may
    call, and the metrics.  A token no identity has, or none where no role is
    named for how the client connects, is refused on the head.  An
    identity's calls share one principal, whichever of its tokens they show.
    The configuration's mistakes are named."""
    def digest(token):
        return "sha256:" + hashlib.sha256(token.encode()).hexdigest()

    def auth(token, **fields):
        return ({"Authorization": f"Bearer {token}"} if token else {}) | fields

    roles = {"admin": {"tools": ["ask_ai", "man"], "metrics": True},
             "asker": {"tools": ["ask_ai"]}, "reader": {"tools": ["man"]},
             "none": {"tools": [], "metrics": False}}
    identities = {
        "alice": {"role": "admin", "tokens": [digest("alice-1")]},
        "bob": {"role": "asker", "tokens": ["env:BOB_TOKEN", digest("bob-2")]},
        "carol": {"role": "reader", "tokens": [digest("carol-1")]},
        "dave.d@x": {"role": "none", "tokens": [digest("dave-1")]}}
    env = {"BOB_TOKEN": "bob-1", "ASKAI_TOKEN": "ignored"}
    with tempfile.TemporaryDirectory() as d:
        path = f"{d}/askai.json"

        def write(**policy):
            with open(path, "w") as f:
                json.dump({"url": up.url(), "man": True} | policy, f)
            os.chmod(path, 0o600)

        write(roles=roles, identities=identities,
              anonymous={"loopback": "reader", "remote": None})
        with Askai(binary, "-c", path, "--slots", "1", env=env) as ai:
            assert any("ASKAI_TOKEN is ignored" in line for line in ai.log), ai.log
            for token, tools in (("alice-1", ["ask_ai", "man"]),
                                 ("bob-1", ["ask_ai"]), ("bob-2", ["ask_ai"]),
                                 ("carol-1", ["man"]), ("dave-1", []),
                                 (None, ["man"])):
                status, _, body = ai.rpc("tools/list", headers=auth(token))
                assert status == 200 and \
                    [t["name"] for t in body["result"]["tools"]] == tools, (token, body)
            for token, tool, message in (
                    ("bob-1", "man", "Tool not available: man"),
                    ("carol-1", "ask_ai", "Tool not available: ask_ai"),
                    (None, "ask_ai", "Tool not available: ask_ai"),
                    ("dave-1", "man", "Tool not available: man"),
                    ("alice-1", "nope", "Unknown tool: nope")):
                _, _, body = ai.rpc("tools/call", {"name": tool, "arguments": {
                    "prompt": "Q", "name": "printf"}}, headers=auth(token))
                assert body["error"]["message"] == message, (token, tool, body)
            for token in ("alice-1", "bob-2"):
                _, _, body = ai.rpc("tools/call", {"name": "ask_ai", "arguments": {
                    "prompt": "Q"}}, headers=auth(token))
                assert text_of(body["result"]) == "A", (token, body)
            _, _, body = ai.rpc("tools/call", {"name": "man", "arguments": {
                "name": "printf"}}, headers=auth("carol-1"))
            assert "result" in body, body
            assert ai.get("/metrics", auth("alice-1"))[0] == 200
            for token in ("bob-1", "carol-1", None):
                assert ai.get("/metrics", auth(token))[0] == 403, token
            # Refused on the head: an unknown token, and none from a client
            # that names another host, as a page rebound to 127.0.0.1 would.
            for headers in (auth("alice-2"), auth("ALICE-1"),
                            auth(None, Host="example.com"),
                            {"Authorization": "Bearer"}):
                status, _, _ = ai.rpc("tools/list", headers=headers)
                assert status == 401, headers
            status, _, _ = ai.rpc("tools/list", headers=auth("alice-1",
                                                             Host="example.com"))
            assert status == 200

            # Bob's calls are one principal's, whichever token they show and
            # whatever session they are of.
            hold = up.reply
            up.reply = lambda request: (200, ANSWERS["openai"], 1.0 if b"HOLD" in
                                        request["raw"] else 0)
            socks = []
            for token, prompt, session in (("alice-1", "HOLD", None),
                                           ("bob-1", "B1", None),
                                           ("bob-2", "B2", "b" * 32),
                                           ("alice-1", "A1", None)):
                socks.append(socket.create_connection(("127.0.0.1", ai.port), 10))
                fields = {"Mcp-Session-Id": session} if session else {}
                socks[-1].sendall(rpc_request(len(socks), {"name": "ask_ai",
                                  "arguments": {"prompt": prompt}}, "tools/call",
                                  headers=auth(token, **fields)))
                if prompt == "B2":
                    wait_for(lambda: ai.metrics(auth("alice-1"))[
                        "askai_jobs_queued"] == 2)
                    assert ai.metrics(auth("alice-1"))["askai_principals_queued"] == 1
            wait_for(lambda: ai.metrics(auth("alice-1"))["askai_jobs_queued"] == 3)
            assert ai.metrics(auth("alice-1"))["askai_principals_queued"] == 2
            for sock in socks:
                sock.settimeout(10)
                assert text_of(Replies(sock).next()[1]["result"]) == "A"
                sock.close()
            up.reply = hold

        # Nobody without a token, unless named.
        write(roles=roles, identities=identities)
        with Askai(binary, "-c", path, env=env) as ai:
            assert ai.rpc("tools/list")[0] == 401
            assert ai.rpc("tools/list", headers=auth("bob-2"))[0] == 200

        for policy, message in (
                ({"roles": []}, "roles must be an object"),
                ({"roles": {"a b": {}}}, "invalid role name: a b"),
                ({"roles": {"r": {"tools": ["ask_ai", "ls"]}}},
                 'role r: askai offers no tool "ls"'),
                ({"roles": {"r": {"tools": "ask_ai"}}}, "role r: tools must be"),
                ({"roles": {"r": {"metrics": 1}}}, "role r: metrics must be"),
                ({"roles": {"r": {"admin": True}}}, "role r: unknown member admin"),
                ({"roles": roles, "identities": {"e": {"role": "root",
                                                       "tokens": ["env:BOB_TOKEN"]}}},
                 'identity e: no role "root"'),
                ({"roles": roles, "identities": {"e": {"role": "admin"}}},
                 "identity e needs tokens"),
                ({"roles": roles, "identities": {"e": {"role": "admin",
                                                       "tokens": ["plain"]}}},
                 "identity e: a token is sha256:HEX or env:NAME"),
                ({"roles": roles, "identities": {"e": {"role": "admin",
                                                       "tokens": ["sha256:00"]}}},
                 "identity e: sha256: needs 64 hex digits"),
                ({"roles": roles, "identities": {"e": {"role": "admin",
                                                       "tokens": ["env:NO_VAR"]}}},
                 "identity e: NO_VAR is not set"),
                ({"roles": roles, "identities": {"e": {"role": "admin",
                                                       "tokens": [digest("bob-1")]},
                                                 "f": {"role": "admin",
                                                       "tokens": ["env:BOB_TOKEN"]}}},
                 "two tokens of the identities are the same"),
                ({"roles": roles, "identities": {"e": {"role": "admin", "uid": 0,
                                                       "tokens": ["env:BOB_TOKEN"]}}},
                 "identity e: unknown member uid"),
                ({"roles": roles, "anonymous": {"local": "admin"}},
                 "anonymous: unknown member local"),
                ({"roles": roles, "anonymous": {"remote": "root"}},
                 'anonymous: remote: no role "root"')):
            write(**policy)
            run = subprocess.run([binary, "-l", "127.0.0.1:0", "-c", path],
                                 env={k: v for k, v in os.environ.items()
                                      if k not in KEY_VARS} | env,
                                 capture_output=True, text=True, timeout=5)
            assert run.returncode == 2 and f"{path}: {message}" in run.stderr, \
                (policy, run.stderr)
        with open(path, "w") as f:
            json.dump({"url": up.url(), "roles": {"r": {"tools": ["man"]}}}, f)
        run = subprocess.run([binary, "-l", "127.0.0.1:0", "-c", path],
                             capture_output=True, text=True, timeout=5)
        assert run.returncode == 2 and 'askai offers no tool "man"' in run.stderr

    print("identities: roles' tools and metrics, tokens, anonymous clients, one "
          "principal per identity, mistakes named: PASS")


def check_reload(binary, up):
    """SIGHUP, with a configuration file, reloads who may connect and what
    they may do, for the requests that begin from then on, while others go
    on: a wrong file leaves the last policy, and other options changed take
    a restart, as the log says.  Without a file, SIGHUP still stops askai."""
    def digest(token):
        return "sha256:" + hashlib.sha256(token.encode()).hexdigest()

    def auth(token):
        return {"Authorization": f"Bearer {token}"} if token else {}

    def tools(ai, token):
        status, _, body = ai.rpc("tools/list", headers=auth(token))
        return [t["name"] for t in body["result"]["tools"]] if status == 200 \
            else status

    full = {"tools": ["ask_ai", "man"], "metrics": True}
    first = {"roles": {"a": full}, "identities": {"alice": {"role": "a", "tokens":
                                                            [digest("t1")]}}}
    second = {"roles": {"a": {"tools": ["man"], "metrics": True},
                        "b": {"tools": ["ask_ai"]}},
              "identities": {"alice": {"role": "a", "tokens": [digest("t1")]},
                             "bob": {"role": "b", "tokens": [digest("t2")]}},
              "anonymous": {"loopback": "b"}}
    with tempfile.TemporaryDirectory() as d:
        path = f"{d}/askai.json"

        def write(policy, **options):
            text = json.dumps({"url": up.url(), "man": True} | options | policy)
            with open(f"{path}.new", "w") as f:
                f.write(text)
            os.chmod(f"{path}.new", 0o600)
            os.rename(f"{path}.new", path)

        def reload(ai, pattern):
            ai.log.clear()
            ai.proc.send_signal(signal.SIGHUP)
            return ai.wait_log(pattern)

        write(first)
        with Askai(binary, "-c", path) as ai:
            assert (tools(ai, "t1"), tools(ai, "t2"), tools(ai, None)) == \
                (["ask_ai", "man"], 401, 401)
            write(second)
            reload(ai, "configuration reloaded: who may connect")
            assert (tools(ai, "t1"), tools(ai, "t2"), tools(ai, None)) == \
                (["man"], ["ask_ai"], ["ask_ai"])
            # A wrong file, or a policy that does not hold, leaves the last.
            with open(path, "a") as f:
                f.write("x")
            reload(ai, "configuration not reloaded; the last stays")
            write({"roles": {"a": full}, "anonymous": {"remote": "z"}})
            assert 'anonymous: remote: no role "z"' in \
                reload(ai, "no role").strip() + "".join(ai.log)
            ai.wait_log("configuration not reloaded")
            assert (tools(ai, "t1"), tools(ai, "t2")) == (["man"], ["ask_ai"])
            # Other options keep, until a restart.
            write(first, **{"tool-description": "New."})
            line = reload(ai, "configuration reloaded")
            assert line.strip().endswith("its other options take a restart"), line
            _, _, body = ai.rpc("tools/list", headers=auth("t1"))
            assert body["result"]["tools"][0]["description"] != "New.", body
            # The roles name the tools that askai runs, not those the file
            # now asks for: man, dropped from the file, runs until a restart,
            # and a new name for the ask tool is no tool yet.
            write(first, man=False)
            line = reload(ai, "configuration reloaded")
            assert line.strip().endswith("its other options take a restart"), line
            assert tools(ai, "t1") == ["ask_ai", "man"]
            write({"roles": {"a": {"tools": ["ask_twin"]}}}, **{"tool-name": "ask_twin"})
            assert 'askai offers no tool "ask_twin"' in \
                reload(ai, "offers no tool").strip() + "".join(ai.log)
            ai.wait_log("configuration not reloaded")
            assert tools(ai, "t1") == ["ask_ai", "man"]
            # A reload refuses a name for the ask tool that another of
            # askai's tools has, as a start does.
            write(first, **{"tool-name": "man"})
            assert "--tool-name: askai has another tool named man" in \
                reload(ai, "another tool named").strip() + "".join(ai.log)
            ai.wait_log("configuration not reloaded")
            assert tools(ai, "t1") == ["ask_ai", "man"]

            # Reloads under way while a client asks: every answer is one of
            # the two policies', and none fails.
            seen = set()
            errors = []
            stop = threading.Event()

            def ask():
                conn = ai.connect()
                while not stop.is_set():
                    status, _, body = ai.rpc("tools/list", headers=auth("t1"),
                                             conn=conn)
                    if status != 200:
                        errors.append((status, body))
                    else:
                        seen.add(tuple(t["name"] for t in body["result"]["tools"]))
                conn.close()

            threads = [threading.Thread(target=ask) for _ in range(4)]
            for thread in threads:
                thread.start()
            for i in range(20):
                write(second if i % 2 else first)
                reload(ai, "configuration reloaded")
            stop.set()
            for thread in threads:
                thread.join(10)
            assert not errors and seen == {("ask_ai", "man"), ("man",)}, (errors, seen)

    # Without a file, SIGHUP stops askai, as SIGTERM does.
    ai = Askai(binary, "-u", up.url())
    ai.proc.send_signal(signal.SIGHUP)
    assert ai.proc.wait(5) == 0, "".join(ai.log)
    print("reload: SIGHUP with a configuration file reloads who may connect, "
          "under load; wrong files and other options logged: PASS")


def uds_rpc(path, method="tools/list", params=None, headers=None):
    """A JSON-RPC request over the Unix socket at @p path: (status, body)."""
    with socket.socket(socket.AF_UNIX) as sock:
        sock.settimeout(10)
        sock.connect(path)
        sock.sendall(rpc_request(1, params, method, headers=headers))
        status, body = Replies(sock).next()
    return status, body


def check_unix(binary, up):
    """A Unix socket beside TCP (--listen unix:PATH): its clients are who the
    kernel says (SO_PEERCRED, SO_PEERGROUPS), an identity's Unix user or
    else a user of a group the policy names, a token still counting first,
    and askai's own policy lets them do everything.  A socket file left by a
    server gone is taken over, one a server listens on is not, and askai
    removes its own as it stops."""
    me = pwd.getpwuid(os.getuid()).pw_name
    group = grp.getgrgid(os.getgid()).gr_name

    def names(reply):
        status, body = reply
        return [t["name"] for t in body["result"]["tools"]] if status == 200 \
            else status

    def digest(token):
        return "sha256:" + hashlib.sha256(token.encode()).hexdigest()

    with tempfile.TemporaryDirectory() as d:
        sock = f"{d}/askai.sock"
        path = f"{d}/askai.json"

        def write(**policy):
            with open(path, "w") as f:
                json.dump({"url": up.url(), "man": True} | policy, f)
            os.chmod(path, 0o600)

        roles = {"a": {"tools": ["ask_ai", "man"], "metrics": True},
                 "g": {"tools": ["man"]}, "t": {"tools": ["ask_ai"]}}
        # askai's own policy: everything, as on loopback.
        with Askai(binary, "-u", up.url(), "--man", "-l", f"unix:{sock}") as ai:
            assert names(uds_rpc(sock)) == ["ask_ai", "man"]
            assert ai.rpc("ping")[0] == 200
            assert any(f"unix:{sock}" in line for line in ai.log), ai.log
        assert not os.path.exists(sock)

        for policy, tools in (
                ({"identities": {"me": {"role": "a", "unix-users": [me]}},
                  "unix-groups": {group: "g"}}, ["ask_ai", "man"]),
                ({"identities": {"me": {"role": "a", "unix-users": [os.getuid()]}}},
                 ["ask_ai", "man"]),
                ({"unix-groups": {group: "g", "root": "a"}}, ["man"]),
                ({"unix-groups": {str(os.getgid()): "g"}}, ["man"]),
                ({"anonymous": {"unix": "t"}}, ["ask_ai"]),
                ({"anonymous": {"loopback": "a"}}, 401)):
            write(roles=roles, **policy)
            with Askai(binary, "-c", path, "-l", f"unix:{sock}") as ai:
                assert names(uds_rpc(sock)) == tools, (policy, uds_rpc(sock))
        write(roles=roles, identities={"tok": {"role": "t", "tokens": [digest("x")]},
                                       "me": {"role": "g", "unix-users": [me]}})
        with Askai(binary, "-c", path, "-l", f"unix:{sock}") as ai:
            assert names(uds_rpc(sock)) == ["man"]
            assert names(uds_rpc(sock, headers={"Authorization": "Bearer x"})) == \
                ["ask_ai"]
            assert names(uds_rpc(sock, headers={"Authorization": "Bearer y"})) == 401
            # The other listener serves as before.
            assert ai.rpc("tools/list")[0] == 401

        # A file left behind is taken over; a socket in use is not.
        with socket.socket(socket.AF_UNIX) as left:
            left.bind(sock)
        with Askai(binary, "-u", up.url(), "-l", f"unix:{sock}") as ai:
            run = subprocess.run([binary, "-u", up.url(), "-l", f"unix:{sock}"],
                                 capture_output=True, text=True, timeout=10)
            assert run.returncode == 1 and "Address already in use" in run.stderr, run
            assert names(uds_rpc(sock)) == ["ask_ai"]
        for policy, message in (
                ({"identities": {"e": {"role": "a",
                                       "unix-users": ["no_such_user_xyz"]}}},
                 'identity e: no user "no_such_user_xyz"'),
                ({"unix-groups": {"no_such_group_xyz": "a"}},
                 "unix-groups: no group no_such_group_xyz"),
                ({"unix-groups": {group: "z"}}, f'unix-groups: {group}: no role "z"'),
                ({"identities": {"e": {"role": "a", "unix-users": [me]},
                                 "f": {"role": "g", "unix-users": [os.getuid()]}}},
                 f"two identities are Unix user {os.getuid()}"),
                ({"identities": {"e": {"role": "a", "unix-users": me}}},
                 "identity e: tokens and unix-users are arrays")):
            write(roles=roles, **policy)
            run = subprocess.run([binary, "-c", path], capture_output=True, text=True,
                                 timeout=5)
            assert run.returncode == 2 and f"{path}: {message}" in run.stderr, \
                (policy, run.stderr)
        run = subprocess.run([binary, "-u", up.url(), "-l", "unix:" + "x" * 200],
                             capture_output=True, text=True, timeout=5)
        assert run.returncode == 1 and "invalid listen address" in run.stderr, run
    print("unix sockets: SO_PEERCRED users and groups, tokens first, askai's own "
          "policy, stale and busy socket files: PASS")


def tls_files(d, name, kind="ec"):
    """A self-signed certificate for 127.0.0.1, of a key of @p kind, and the
    key, in @p d: (certificate's file, key's file)."""
    newkey = {"ec": ["ec", "-pkeyopt", "ec_paramgen_curve:P-256"],
              "ed25519": ["ed25519"], "rsa": ["rsa:2048"]}[kind]
    crt, key = f"{d}/{name}.crt", f"{d}/{name}.key"
    subprocess.run(["openssl", "req", "-x509", "-newkey", *newkey, "-noenc",
                    "-keyout", key, "-out", crt, "-days", "1", "-subj",
                    f"/CN={name}", "-addext", "subjectAltName=IP:127.0.0.1"],
                   check=True, capture_output=True)
    os.chmod(crt, 0o644)
    os.chmod(key, 0o600)
    return crt, key


def tls_client(crt, alpn=("http/1.1",), top=ssl.TLSVersion.TLSv1_3):
    """A client's TLS context that trusts @p crt, offers @p alpn, and speaks
    TLS up to @p top."""
    ctx = ssl.create_default_context(cafile=crt)
    ctx.maximum_version = top
    if alpn:
        ctx.set_alpn_protocols(list(alpn))
    return ctx


def tls_port(ai):
    """The port of @p ai's TLS listener, as its log says."""
    line = ai.wait_log(r"MCP endpoint https://")
    return int(re.search(r":(\d+)/mcp$", line.strip()).group(1))


def tls_connect(ctx, port, rcvbuf=None):
    """A TLS connection to @p port, its handshake done, with a receive
    buffer of @p rcvbuf bytes if given."""
    sock = socket.socket()
    if rcvbuf:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcvbuf)
    sock.settimeout(10)
    sock.connect(("127.0.0.1", port))
    return ctx.wrap_socket(sock, server_hostname="127.0.0.1")


def check_tls(binary, up):
    """TLS listeners (tls:): TLS 1.3 and ALPN http/1.1 only; records that
    come together, requests and answers of many records, records' rests
    that TLS holds, a key update; a handshake within the head's deadline,
    plain HTTP and the cap answered with a close; a certificate and key
    checked as askai starts, and reloaded on SIGHUP without a configuration
    file."""
    def fails(*args, message):
        run = subprocess.run([binary, "-l", "127.0.0.1:0", "-u", up.url(), *args],
                             stdin=subprocess.DEVNULL, capture_output=True,
                             text=True, timeout=5, check=False)
        assert run.returncode == 2 and message in run.stderr, (args, run.stderr)

    def https(ctx, port):
        return http.client.HTTPSConnection("127.0.0.1", port, timeout=10,
                                           context=ctx)

    def refused(ctx, port):
        try:
            with tls_connect(ctx, port):
                return False
        except ssl.SSLError:
            return True

    with tempfile.TemporaryDirectory() as d:
        crt, key = tls_files(d, "one")
        tls = ("-l", "tls:127.0.0.1:0", "--tls-cert", crt, "--tls-key", key)
        with Askai(binary, "-u", up.url(), *tls, "--head-timeout", "1",
                   "--idle-timeout", "2") as ai:
            port = tls_port(ai)
            ctx = tls_client(crt)
            conn = https(ctx, port)
            status, _, body = ai.rpc("tools/list", conn=conn)
            assert status == 200 and body["result"]["tools"], (status, body)
            assert (conn.sock.version(), conn.sock.selected_alpn_protocol()) == \
                ("TLSv1.3", "http/1.1")
            conn.close()
            status, _, _ = ai.rpc("ping", conn=https(tls_client(crt, alpn=()), port))
            assert status == 200, status

            # Requests whose records come in one segment, which TLS reads a
            # record at a time: each is answered.
            with tls_connect(ctx, port) as sock:
                sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_CORK, 1)
                for i in range(5):
                    sock.sendall(rpc_request(i))
                sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_CORK, 0)
                replies = Replies(sock)
                assert [replies.next()[1]["id"] for _ in range(5)] == list(range(5))

            # A question and its answer of many records each: the question's
            # records come whole, and reads with less room leave their rests
            # in TLS, which epoll does not see; the answer goes to a client
            # that reads it late, through a small window, so that sends wait
            # and are sent again.
            answer = "x" * (600 << 10)
            up.reply = lambda request: (200, {"choices": [{"message": {
                "role": "assistant", "content": answer}, "finish_reason": "stop"}]})
            try:
                with tls_connect(ctx, port, rcvbuf=4096) as sock:
                    sock.sendall(rpc_request(1, {"name": "ask_ai", "arguments": {
                        "prompt": "y" * (700 << 10)}}, "tools/call"))
                    time.sleep(0.5)
                    status, body = Replies(sock).next()
                    assert status == 200 and text_of(body["result"]) == answer, status
            finally:
                up.reply = lambda request: (200, ANSWERS["openai"])
            assert len(up.last()["body"]["messages"][-1]["content"]) == 700 << 10

            # A key update the client asks for is answered, and requests go on.
            with subprocess.Popen(["openssl", "s_client", "-connect",
                                   f"127.0.0.1:{port}", "-alpn", "http/1.1",
                                   "-CAfile", crt], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT) as client:
                # It reads its input once its handshake is done, and a K must
                # come alone.
                seen = b""
                end = time.monotonic() + 10
                while b"Verify return code" not in seen and time.monotonic() < end:
                    if select.select([client.stdout], [], [], 0.2)[0]:
                        seen += os.read(client.stdout.fileno(), 65536)
                for line in (b"K\n", rpc_request(7), b"K\n", rpc_request(8)):
                    client.stdin.write(line)
                    client.stdin.flush()
                    time.sleep(0.3)
                    while select.select([client.stdout], [], [], 0.2)[0]:
                        chunk = os.read(client.stdout.fileno(), 65536)
                        if not chunk:
                            break
                        seen += chunk
                client.stdin.close()
                seen += client.stdout.read()
            assert seen.count(b"KEYUPDATE") == 2 and b'"id":7' in seen and \
                b'"id":8' in seen, seen.decode(errors="replace")

            # TLS 1.2, or no protocol but h2, fails the handshake; plain HTTP
            # fails it at its first bytes, and half a ClientHello at the
            # head's deadline: each closed with no answer.
            assert refused(tls_client(crt, top=ssl.TLSVersion.TLSv1_2), port)
            assert refused(tls_client(crt, alpn=("h2",)), port)
            with socket.create_connection(("127.0.0.1", port), 10) as sock:
                sock.sendall(rpc_request(1))
                assert closed_by_peer(sock), "plain HTTP answered"
            with socket.create_connection(("127.0.0.1", port), 10) as sock:
                sock.sendall(b"\x16\x03\x01")
                start = time.monotonic()
                assert closed_by_peer(sock, 4) and time.monotonic() - start < 3
            # A client that says nothing has the idle deadline, and one that
            # closes without a word began no handshake.
            with socket.create_connection(("127.0.0.1", port), 10) as sock:
                start = time.monotonic()
                assert closed_by_peer(sock, 5) and time.monotonic() - start > 1.5
            socket.create_connection(("127.0.0.1", port), 10).close()
            time.sleep(0.2)
            m = ai.metrics()
            assert total(m, "askai_tls_handshakes_total", result="failed") == 4, m
            assert total(m, "askai_tls_handshakes_total", result="done") >= 5, m
            assert total(m, "askai_tls_flight_seconds_count") >= 10, m

        # The cap closes a TLS client with nothing said.
        with Askai(binary, "-u", up.url(), *tls, "--max-client-conns", "1") as ai:
            port = tls_port(ai)
            with tls_connect(tls_client(crt), port) as first:
                with socket.create_connection(("127.0.0.1", port), 10) as sock:
                    assert sock.recv(64) == b""
                first.sendall(rpc_request(3))
                assert Replies(first).next()[1]["id"] == 3

        # SIGHUP reloads the certificate and key, without a configuration
        # file; those who shook hands go on.  A key that is not the
        # certificate's leaves the last.
        with Askai(binary, "-u", up.url(), *tls) as ai:
            port = tls_port(ai)
            one = tls_client(crt)
            old = https(one, port)
            assert ai.rpc("ping", conn=old)[0] == 200
            crt2, key2 = tls_files(d, "two")
            os.replace(crt2, crt)
            os.replace(key2, key)
            ai.log.clear()
            ai.proc.send_signal(signal.SIGHUP)
            ai.wait_log("configuration reloaded: .*, and the TLS certificate$")
            two = tls_client(crt)
            assert ai.rpc("ping", conn=https(two, port))[0] == 200
            assert refused(one, port)
            assert ai.rpc("ping", conn=old)[0] == 200
            os.replace(tls_files(d, "three")[1], key)
            ai.log.clear()
            ai.proc.send_signal(signal.SIGHUP)
            ai.wait_log("configuration not reloaded; the last stays")
            assert "TLS: the key is not the certificate's" in "".join(ai.log)
            assert ai.rpc("ping", conn=https(two, port))[0] == 200
            old.close()

        # Ed25519, and RSA if allowed.
        for kind, extra in (("ed25519", ()), ("rsa", ("--tls-allow-rsa",))):
            c, k = tls_files(d, kind, kind)
            with Askai(binary, "-u", up.url(), "-l", "tls:127.0.0.1:0",
                       "--tls-cert", c, "--tls-key", k, *extra) as ai:
                assert ai.rpc("ping", conn=https(tls_client(c), tls_port(ai)))[0] == 200

        # What askai refuses to start with.
        crt, key = tls_files(d, "four")
        rsa_crt, rsa_key = f"{d}/rsa.crt", f"{d}/rsa.key"
        enc = f"{d}/enc.key"
        subprocess.run(["openssl", "genpkey", "-algorithm", "ec", "-pkeyopt",
                        "ec_paramgen_curve:P-256", "-aes256", "-pass", "pass:x",
                        "-out", enc], check=True, capture_output=True)
        os.chmod(enc, 0o600)
        listen = ("-l", "tls:127.0.0.1:0")
        fails(*listen, message="a tls: listener needs --tls-cert and --tls-key")
        fails(*listen, "--tls-cert", crt, message="needs --tls-cert and --tls-key")
        fails("-l", "tls:unix:/nonexistent/askai", "--tls-cert", crt, "--tls-key", key,
              message="TLS over a Unix socket")
        fails(*listen, "--tls-cert", f"{d}/none", "--tls-key", key,
              message="cannot open")
        fails(*listen, "--tls-cert", key, "--tls-key", key,
              message="TLS: the certificate chain: ")
        fails(*listen, "--tls-cert", crt, "--tls-key", rsa_key,
              message="TLS: the key is not the certificate's")
        fails(*listen, "--tls-cert", rsa_crt, "--tls-key", rsa_key,
              message="the key is RSA, which needs --tls-allow-rsa")
        fails(*listen, "--tls-cert", crt, "--tls-key", enc,
              message="TLS: the key is encrypted")
        os.chmod(key, 0o644)
        fails(*listen, "--tls-cert", crt, "--tls-key", key,
              message=f"{key}: others may read it")
        os.chmod(key, 0o640)
        with Askai(binary, "-u", up.url(), *listen, "--tls-cert", crt,
                   "--tls-key", key):
            pass
    print("tls: TLS 1.3 and http/1.1 only, records held, waits and key "
          "updates, deadlines, plain HTTP, the cap, certificate checks and "
          "reload: PASS")


def local_address():
    """A non-loopback address of this host, or None."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        try:
            sock.connect(("192.0.2.1", 9))
        except OSError:
            return None
        addr = sock.getsockname()[0]
    return None if addr.startswith("127.") else addr


def check_metrics(binary, up):
    """GET /metrics gives Prometheus text to a client on loopback, or to one
    with the token when one is set, and to nobody else."""
    with Askai(binary, "-u", up.url()) as ai:
        assert ai.rpc("ping")[0] == 200
        assert text_of(ai.ask()) == "A"
        ai.post(b"{not json")
        values = ai.metrics()
        loops = [f'loop="{k}"' for k in range(1, 5)] + ['loop="home"']
        for series in ("askai_loop_turn_seconds_count", "askai_stash_retained_bytes"):
            assert [x for x in values if x.startswith(series + "{")] == \
                [f"{series}{{{loop}}}" for loop in loops], values
        assert [x for x in values if x.startswith("askai_connections{")] == \
            [f"askai_connections{{{loop}}}" for loop in loops[:4]], values
        for series, labels, least in (("askai_loop_turn_seconds_count", {}, 1),
                                      ("askai_loop_step_seconds_count",
                                       {"kind": "interactive"}, 1),
                                      ("askai_loop_step_work_max", {"unit": "reqs"}, 1),
                                      ("askai_request_seconds_count",
                                       {"method": "ping"}, 1),
                                      ("askai_request_seconds_count",
                                       {"method": "tools/call"}, 1),
                                      ("askai_request_seconds_count",
                                       {"method": "other"}, 1),
                                      ("askai_connections_placed_total", {}, 4),
                                      ("askai_connections", {}, 1),
                                      ("askai_upstream_wait_seconds_count", {}, 1),
                                      ("askai_upstream_run_seconds_count", {}, 1),
                                      ("askai_resident_bytes", {}, 1 << 20),
                                      ("askai_open_fds", {}, 4)):
            assert total(values, series, **labels) >= least, (series, labels, values)
        assert values["askai_max_fds"] >= values["askai_open_fds"]
        # The descriptor table was grown before threads shared it: growing it
        # after makes the kernel wait for an RCU grace period, in a step.
        with open(f"/proc/{ai.proc.pid}/status") as f:
            size = int(re.search(r"FDSize:\s+(\d+)", f.read()).group(1))
        assert size >= min(values["askai_max_fds"], 65536), size
        assert values["askai_log_dropped_total"] == 0
        assert values['askai_upstream_lookups_total{result="ok"}'] == 0
        # Home's scheduler and upstream: one call went upstream, streamed, and
        # nothing waits.
        assert (values["askai_jobs_dispatched_total"], values["askai_upstream_slots"],
                values["askai_jobs_queued"], values["askai_principals_queued"],
                values["askai_upstream_calls"], values["askai_breaker_state"]) == \
            (1, 4, 0, 0, 0, 0), values
        assert values["askai_upstream_chunks_total"] >= 3, values
        # Rendering one is a request too.
        assert total(ai.metrics(), "askai_request_seconds_count", method="metrics") >= 1
        assert ai.get("/metrics?x=1")[0] == 200
        assert ai.get("/mcp")[0] == 405
        # A page whose name was rebound to 127.0.0.1 names its own host, and
        # its GET carries no Origin.
        for host, expected in ((f"evil.example:{ai.port}", 403),
                               ("localhost.evil.example", 403),
                               (f"localhost:{ai.port}", 200), ("[::1]", 200)):
            assert ai.get("/metrics", {"Host": host})[0] == expected, host
        assert ai.raw(b"GET /metrics HTTP/1.0\r\n\r\n").startswith(
            b"HTTP/1.1 200 "), "a client that names no host"

    with Askai(binary, "-u", up.url(), env={"ASKAI_TOKEN": "s3cret"}) as ai:
        assert ai.get("/metrics")[0] == 401
        assert "askai_open_fds" in ai.metrics({"Authorization": "Bearer s3cret"})
        assert ai.get("/metrics", {"Authorization": "Bearer s3cret",
                                   "Host": "evil.example"})[0] == 200

    addr = local_address()
    if addr:
        for env, auth, expected in (({}, {}, 403),
                                    ({"ASKAI_TOKEN": "s3cret"}, {}, 401),
                                    ({"ASKAI_TOKEN": "s3cret"},
                                     {"Authorization": "Bearer s3cret"}, 200)):
            with Askai(binary, "-l", f"{addr}:0", "-u", up.url(), env=env) as ai:
                status, _, text = ai.get("/metrics", auth, source=(addr, 0))
                assert status == expected, (env, auth, status, text)

    print("metrics: Prometheus text, for loopback or the token only: PASS")


def check_memory(binary, up):
    """Buffers' storage goes back to the loop's stash and is taken from there
    again, so steady requests make no allocator calls for it, and nothing goes
    back to the system without memory pressure.  Under it, a relief at a time
    gives back, in bounded steps, what the level says: at high pressure what
    went unused for a window (10 s), at critical all the stash keeps; then the
    allocator is trimmed.  While pressure is critical, storage given up goes
    back at once, and connections that wait shrink their buffers to their
    bytes, which requests read on correctly.  The pressure comes from resident
    memory against --memory-high, from the system's and the cgroup's memory,
    or from the test aid's file."""
    loop = '{loop="1"}'
    home = '{loop="home"}'
    hits = 'askai_stash_takes_total{loop="1",result="hit"}'
    misses = 'askai_stash_takes_total{loop="1",result="miss"}'
    retained = "askai_stash_retained_bytes" + loop
    released = "askai_stash_released_bytes_total" + loop
    reliefs = "askai_memory_reliefs_total" + loop

    def trims(values):
        """The trims every loop ran: the last loop of a relief runs one."""
        return total(values, "askai_memory_trims_total")
    with open("/proc/meminfo") as f:
        ram = int(re.search(r"MemTotal:\s+(\d+) kB", f.read()).group(1)) << 10
    # Real pressure would add reliefs of its own.
    calm = memory_calm()
    level = tempfile.NamedTemporaryFile("w", prefix="askai-pressure-")
    set_pressure(level, 0)
    call = {"name": "ask_ai", "arguments": {"prompt": "Q"}}
    with Askai(binary, "-u", up.url(), "--test-pressure", level.name,
               "--max-client-conns", "0", "--output-pause", "16K", loops=1) as ai:
        started = time.monotonic()
        conn = ai.connect()

        def burst(n):
            for _ in range(n):
                assert ai.rpc("ping", conn=conn)[0] == 200
                assert ai.rpc("tools/call", call, conn=conn)[0] == 200

        burst(20)
        ai.metrics()  # its reply grows as it is rendered
        before = ai.metrics()
        burst(100)
        after = ai.metrics()
        assert after[hits] - before[hits] >= 800, (before[hits], after[hits])
        # Rarely, a turn holds more storage at once than any before it.
        assert after[misses] - before[misses] <= 2, (before[misses], after[misses])
        assert after[retained] > 0
        assert after["askai_memory_total_bytes"] == ram
        assert 0 < after["askai_memory_available_bytes"] <= \
            after["askai_memory_total_bytes"]
        assert after["askai_memory_high_bytes"] >= 256 << 20
        assert after['askai_kept_bytes{loop="1",kind="tokeners"}'] > 0
        if calm:
            assert after["askai_memory_pressure_level"] == 0, after
            assert after[reliefs] == trims(after) == after[released] == 0, after

        # Requests are kept for reuse, eight after eight at once; steady
        # calls after those reuse the one that ended last, so only that one
        # keeps a large answer's text.
        socks = [socket.create_connection(("127.0.0.1", ai.port), 10)
                 for _ in range(8)]
        for i, sock in enumerate(socks):
            sock.sendall(rpc_request(i)[:-1])
        time.sleep(0.2)
        for i, sock in enumerate(socks):
            sock.sendall(rpc_request(i)[-1:])
        for sock in socks:
            assert Replies(sock).next()[0] == 200
            sock.close()
        up.reply = lambda request: (200, {"choices": [{"message": {
            "content": "z" * 100_000}, "finish_reason": "stop"}]})
        for _ in range(16):
            assert ai.rpc("tools/call", call, conn=conn)[0] == 200
        up.reply = lambda request: (200, ANSWERS["openai"])
        values = ai.metrics()
        assert values["askai_spare_requests" + loop] >= 7, values
        assert 0 < values['askai_kept_bytes{loop="1",kind="requests"}'] < 300_000, values
        given = values[released]

        # High: no window has passed, so nothing counts as unused yet, and
        # only the allocator is trimmed; once one has, what the tool calls
        # alone took went unused through it, and goes.
        set_pressure(level, 1)
        wait_for(lambda: trims(ai.metrics()) >= 1)
        values = ai.metrics()
        assert values['askai_memory_pressure_input_level{input="test"}'] == 1
        assert values["askai_memory_pressure_level"] >= 1
        assert not calm or values[reliefs] == 1, values[reliefs]
        assert total(values, "askai_memory_trim_seconds_total") > 0
        assert not calm or values[released] == given, (given, values[released])
        set_pressure(level, 0)
        wait_for(lambda: ai.metrics()["askai_memory_pressure_level"] == 0 or not calm)
        time.sleep(max(0, started + 21 - time.monotonic()))
        set_pressure(level, 1)
        wait_for(lambda: trims(ai.metrics()) >= 2)
        assert ai.metrics()[released] > values[released]
        assert any("memory pressure high" in line for line in ai.log), ai.log

        # Critical: all goes, a bounded share a step, from a stash that
        # connections cut short have filled.
        set_pressure(level, 0)
        wait_for(lambda: ai.metrics()["askai_memory_pressure_level"] == 0 or not calm)
        socks = [socket.create_connection(("127.0.0.1", ai.port), 10)
                 for _ in range(400)]
        for sock in socks:
            sock.sendall(b"POST /mcp HTTP/1.1\r\n")
        time.sleep(0.5)
        for sock in socks:
            sock.close()
        wait_for(lambda: ai.metrics()[retained] >= 400 << 12)
        # Home keeps the storage of answers it took in, and takes part.
        up.reply = lambda request: (200, {"choices": [{"message": {
            "content": "z" * 300_000}, "finish_reason": "stop"}]})
        for _ in range(4):
            assert ai.rpc("tools/call", call, conn=conn)[0] == 200
        up.reply = lambda request: (200, ANSWERS["openai"])
        before = ai.metrics()
        assert before["askai_stash_retained_bytes" + home] >= 300_000, before
        work(ai)
        set_pressure(level, 2)
        wait_for(lambda: trims(ai.metrics()) >= 3)
        values = ai.metrics()
        assert values[retained] < 256 << 10, values[retained]
        assert values[released] >= 400 << 12, values[released]
        assert not values["askai_stash_retained_bytes" + home], values
        assert values["askai_stash_released_bytes_total" + home] >= \
            before["askai_stash_released_bytes_total" + home] + 300_000, values
        assert values["askai_memory_reliefs_total" + home] > \
            before["askai_memory_reliefs_total" + home], values
        units, _ = work(ai)
        assert units["release"] == (256, 256), units

        # A connection that waits for the rest of a pipelined head moves it
        # into a smaller block, and then reads it on.
        with socket.create_connection(("127.0.0.1", ai.port), 10) as sock:
            pad = {"X-Pad": "p" * 2800}
            sock.sendall(rpc_request(1, headers=pad) + rpc_request(2, headers=pad)[:2048])
            replies = Replies(sock)
            assert replies.next()[1]["id"] == 1
            time.sleep(0.2)
            units, _ = work(ai)
            assert units["copy"][0] >= 2000, units
            sock.sendall(rpc_request(2, headers=pad)[2048:])
            assert replies.next() == (200, {"jsonrpc": "2.0", "id": 2, "result": {}})
        assert not ai.metrics()[retained]

        # A tool call that waits for the upstream moves its head, whose
        # session another session's cancellation reads, and so does its own.
        big = "x" * 6_000_000

        def reply(request):
            raw = request["raw"].decode()
            if "SLOW" in raw:
                return 200, ANSWERS["openai"], 5.0
            return 200, {"choices": [{"message": {"content": big if "BIG" in raw
                                                  else "A"}, "finish_reason": "stop"}]}

        up.reply = reply
        _, headers, _ = ai.rpc("initialize", {"protocolVersion": LEGACY})
        session = {"Mcp-Session-Id": headers["mcp-session-id"],
                   "MCP-Protocol-Version": LEGACY, "X-Pad": "p" * 2500}
        up.arrived.clear()
        up.cancelled.clear()
        with socket.create_connection(("127.0.0.1", ai.port), 10) as sock:
            sock.sendall(rpc_request(42, {"name": "ask_ai", "arguments": {
                "prompt": "SLOW" + "y" * 20_000}}, "tools/call", accept_sse=True,
                headers=session))
            assert up.arrived.wait(5)
            time.sleep(0.2)
            for other, cancels in (("f" * 32, False), (session["Mcp-Session-Id"], True)):
                assert ai.rpc("notifications/cancelled", {"requestId": 42}, rpc_id=None,
                              headers={"Mcp-Session-Id": other,
                                       "MCP-Protocol-Version": LEGACY})[0] == 202
                assert up.cancelled.wait(0.5 if not cancels else 5) == cancels
            sock.settimeout(5)
            stream = b""
            while not stream.endswith(b"\r\n0\r\n\r\n"):
                chunk = sock.recv(65536)
                assert chunk, stream
                stream += chunk
            assert stream.startswith(b"HTTP/1.1 200 OK\r\n") and b"data:" not in stream

        # An answer larger than the sockets' buffers, which a slow reader
        # drains, goes out whole, its output moved into smaller blocks as it
        # drains.
        with socket.socket() as sock:
            sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
            sock.connect(("127.0.0.1", ai.port))
            sock.sendall(rpc_request(5, {"name": "ask_ai", "arguments": {
                "prompt": "BIG"}}, "tools/call"))
            time.sleep(0.5)
            status, body = Replies(sock).next()
            assert status == 200 and text_of(body["result"]) == big
        up.reply = lambda request: (200, ANSWERS["openai"])
        trimmed = trims(ai.metrics())
        set_pressure(level, 0)

        # Without pressure, storage is kept again.
        wait_for(lambda: ai.metrics()["askai_memory_pressure_level"] == 0 or not calm)
        burst(10)
        values = ai.metrics()
        assert values[retained] > 0 and trims(values) == trimmed or not calm, values

    # With several loops, a relief has each give back what it keeps, home
    # too, and the last of them to end its share trims the allocator, once
    # for all.
    if calm:
        set_pressure(level, 0)
        with Askai(binary, "-u", up.url(), "--test-pressure", level.name) as ai:
            for _ in range(8):
                assert text_of(ai.ask()) == "A"
            for rounds in (1, 2):
                set_pressure(level, 2)
                wait_for(lambda: trims(ai.metrics()) >= rounds)
                time.sleep(1.5)
                values = ai.metrics()
                assert trims(values) == rounds, values
                assert [v for k, v in values.items()
                        if k.startswith("askai_memory_reliefs_total{")] == \
                    [rounds] * 5, values
                set_pressure(level, 0)
                wait_for(lambda: ai.metrics()["askai_memory_pressure_level"] == 0)
            assert any("memory pressure relieved: " in line for line in ai.log), \
                ai.log

    # Under a data segment limit glibc's heaps keep their own top pad, which
    # leaves room for the work: one as large as a heap would make each
    # thread's heap count whole.  The sanitizers' shadow memory counts too,
    # and so does each client loop, about 2.3 MiB: its stack, its histograms
    # and its log ring.
    with open(binary, "rb") as f:
        exe = f.read()
    if b"__asan_init" not in exe and b"__tsan_init" not in exe:
        body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "ping",
                           "params": {"x": "a" * 1_000_000}}).encode()
        with Askai(binary, "-u", up.url(), data=128 << 20, loops=8) as ai:
            results = []
            threads = [threading.Thread(target=lambda: results.append(
                ai.post(body)[0])) for _ in range(20)]
            for thread in threads:
                thread.start()
            for thread in threads:
                thread.join(10)
            assert results == [200] * 20, results

    # Resident memory far over --memory-high is critical pressure.
    with Askai(binary, "-u", up.url(), "--memory-high", "1M") as ai:
        wait_for(lambda: trims(ai.metrics()) >= 1)
        values = ai.metrics()
        assert values["askai_memory_high_bytes"] == 1 << 20
        assert values['askai_memory_pressure_input_level{input="resident"}'] == 2
        assert values["askai_memory_pressure_level"] == 2

    print("memory: buffers kept for reuse, given back under pressure: PASS")


def check_lookups(binary, up):
    """A blocker looks the upstream host up, and its addresses serve for a
    while; libcurl's own resolver stays idle."""
    url = up.url().replace("127.0.0.1", "localhost")
    with Askai(binary, "-u", url) as ai:
        for _ in range(3):
            assert text_of(ai.ask()) == "A"
        values = ai.metrics()
        assert values['askai_upstream_lookups_total{result="ok"}'] == 1, values
        assert values["askai_blocker_jobs_total"] == 1, values
        assert values['askai_upstream_lookups_total{result="failed"}'] == 0

    # One lookup fails as home asks for /props, and one for the call.
    with Askai(binary, "-u", "http://nonexistent.invalid:1/v1/chat/completions") as ai:
        result = ai.ask()
        assert result["isError"] and \
            "cannot look up nonexistent.invalid: " in text_of(result), result
        assert ai.metrics()['askai_upstream_lookups_total{result="failed"}'] == 2
        ai.wait_log(r"upstream slots: 4 \(the default: /props: cannot look up ")

    print("upstream host lookups on a blocker thread: PASS")


def check_shutdown(binary, up):
    """SIGTERM drains: no new clients, idle ones closed, tool calls ended with
    an error, on an event stream as its last event, other requests answered,
    a body still on its way included; then askai exits 0."""
    up.reply = lambda request: (200, ANSWERS["openai"], 10)
    up.cancelled.clear()
    ai = Askai(binary, "-u", up.url())
    try:
        idle = held_ping(ai.port)
        crowd = [held_ping(ai.port, i) for i in range(100)]
        calls = len(up.requests) + 2
        call = {"name": "ask_ai", "arguments": {"prompt": "SLOW"}}
        sse = socket.create_connection(("127.0.0.1", ai.port), 10)
        sse.sendall(rpc_request(5, call, "tools/call", accept_sse=True))
        assert b"Content-Type: text/event-stream" in sse.recv(4096)
        plain = socket.create_connection(("127.0.0.1", ai.port), 10)
        plain.sendall(rpc_request(6, call, "tools/call"))
        for _ in range(500):
            if len(up.requests) >= calls:
                break
            time.sleep(0.01)
        partial = socket.create_connection(("127.0.0.1", ai.port), 10)
        wire = rpc_request(7)
        partial.sendall(wire[:-10])
        # Metrics whose request is under way, answered after the drain.
        late = socket.create_connection(("127.0.0.1", ai.port), 10)
        late.sendall(b"GET /metrics HTTP/1.1\r\nHost: localhost\r\n"
                     b"Content-Length: 2\r\n\r\n{")
        time.sleep(0.1)

        start = time.monotonic()
        ai.proc.send_signal(signal.SIGTERM)
        time.sleep(0.2)
        partial.sendall(wire[-10:])
        partial.settimeout(5)
        rest = b""
        while chunk := partial.recv(65536):
            rest += chunk
        assert b"Connection: close\r\n" in rest and b'"id":7' in rest, rest
        partial.close()
        status, body = Replies(plain).next()
        assert status == 503 and body["error"]["message"] == \
            "Server is shutting down" and body["id"] == 6, body
        sse.settimeout(5)
        rest = b""
        while chunk := sse.recv(65536):
            rest += chunk
        assert rest.endswith(b"0\r\n\r\n"), rest
        assert b'data: {"jsonrpc":"2.0","id":5,"error":{"code":-32603,' \
               b'"message":"Server is shutting down"}}' in rest, rest
        assert closed_by_peer(idle)
        assert all(closed_by_peer(sock) for sock in crowd)
        # The drain closed them a share of a step at a time.
        late.sendall(b"}")
        late.settimeout(5)
        text = b""
        while chunk := late.recv(65536):
            text += chunk
        values = prometheus(text.split(b"\r\n\r\n", 1)[1].decode())
        closes = [v for k, v in values.items()
                  if k.startswith("askai_loop_step_work_max{") and 'unit="closes"' in k]
        assert 1 < max(closes) <= 32, closes
        for sock in (idle, sse, plain, late, *crowd):
            sock.close()
        assert ai.proc.wait(5) == 0, "".join(ai.log)
        assert time.monotonic() - start < 3
        assert up.cancelled.wait(2), "the upstream call outlived the shutdown"
    finally:
        if ai.proc.poll() is None:
            ai.proc.kill()
            ai.proc.wait()
    with socket.socket() as sock:
        assert sock.connect_ex(("127.0.0.1", ai.port)) != 0, "still listening"
    up.reply = lambda request: (200, ANSWERS["openai"])

    # Clients that come as the drain begins, some of them on their way from
    # home to a client loop then, are each served or closed, none lost or
    # left open: askai is stopped while they connect and ask.  A head part way
    # on every loop holds the drain open until its deadline, so each client
    # must have been closed by askai, not by its exit.
    ai = Askai(binary, "-u", up.url())
    try:
        parts = [socket.create_connection(("127.0.0.1", ai.port), 10)
                 for _ in range(32)]
        for sock in parts:
            sock.sendall(b"POST /mcp HTTP/1.1\r\n")
        time.sleep(0.1)
        conns = [v for k, v in ai.metrics().items()
                 if k.startswith("askai_connections{")]
        assert min(conns) >= 2, conns
        ai.proc.send_signal(signal.SIGSTOP)
        try:
            socks = [socket.create_connection(("127.0.0.1", ai.port), 10)
                     for _ in range(256)]
            for i, sock in enumerate(socks):
                sock.sendall(rpc_request(i))
            ai.proc.send_signal(signal.SIGTERM)
        finally:
            ai.proc.send_signal(signal.SIGCONT)
        for sock in socks:
            sock.settimeout(2)
            rest = b""
            try:
                while chunk := sock.recv(65536):
                    rest += chunk
            except ConnectionResetError:
                pass
            assert not rest or rest.startswith(b"HTTP/1.1 200 "), rest
            sock.close()
        assert ai.proc.poll() is None, "".join(ai.log)
        for sock in parts:
            sock.close()
        assert ai.proc.wait(5) == 0, "".join(ai.log)
    finally:
        if ai.proc.poll() is None:
            ai.proc.kill()
            ai.proc.wait()

    # Ctrl-C does the same.
    ai = Askai(binary, "-u", up.url())
    assert ai.rpc("ping")[0] == 200
    ai.proc.send_signal(signal.SIGINT)
    assert ai.proc.wait(5) == 0

    # Signals ignored from the start, as nohup and shells leave them, stay so.
    with Askai(binary, "-u", up.url(), ignore=(signal.SIGHUP, signal.SIGINT)) as ai:
        for sig in (signal.SIGHUP, signal.SIGINT):
            ai.proc.send_signal(sig)
        time.sleep(0.2)
        assert ai.proc.poll() is None and ai.rpc("ping")[0] == 200

    print("shutdown: draining, tool calls ended with an error: PASS")


def check_streaming(binary, up):
    """Every protocol asks for a streamed answer, and home reads its events
    into the answer's text, however the bytes are cut up on the way, with CRLF
    line ends or JSON over several data lines, and without a line break at the
    end; a plain answer to a streamed request is read as before.  An error
    event ends the call with its message, and the upstream sees the hang-up;
    so does an answer that is not UTF-8 text, an event that is not JSON text,
    or a pause longer than --chunk-timeout once the answer has begun.  One
    that ends before it says the answer has is cut off.  A stop reason is the
    same, streamed or not, and the text is the first completion's.  The
    deadline counts from the start upstream."""
    text = 'Hé, € \U0001f600 "quoted"\n\\ ' * 40
    answers = {
        "openai": {"choices": [{"message": {"content": text},
                                "finish_reason": "stop"}]},
        "responses": {"status": "completed", "output": [
            {"type": "message", "content": [{"type": "output_text", "text": text}]}]},
        "anthropic": {"content": [{"type": "thinking", "thinking": "hmm"},
                                  {"type": "text", "text": text}],
                      "stop_reason": "end_turn"},
        "gemini": {"candidates": [{"content": {"parts": [
            {"text": "hmm", "thought": True}, {"text": text}]},
            "finishReason": "STOP"}]},
    }
    errors = {
        "openai": (None, {"error": {"message": "boom"}}),
        "responses": ("error", {"type": "error", "code": "x", "message": "boom"}),
        "anthropic": ("error", {"type": "error", "error": {
            "type": "overloaded_error", "message": "boom"}}),
        "gemini": (None, {"error": {"code": 500, "message": "boom"}}),
    }
    paths = {"openai": "/v1/chat/completions", "responses": "/v1/responses",
             "anthropic": "/v1/messages", "gemini": "/v1beta/models/m:generateContent"}
    # Stop reasons longer than most, of answers with no text.
    empty = {"openai": {"choices": [{"message": {"content": ""},
                                     "finish_reason": "content_filter"}]},
             "responses": {"status": "incomplete", "output": [],
                           "incomplete_details": {"reason": "content_filter"}},
             "anthropic": {"content": [],
                           "stop_reason": "model_context_window_exceeded"},
             "gemini": {"candidates": [{"content": {"parts": []},
                                        "finishReason": "IMAGE_PROHIBITED_CONTENT"}]}}
    cuts = (1,) * 300 + (7, 13, 2, 31, 5, 64) * 200
    for protocol, answer in answers.items():
        with Askai(binary, "--api", API[protocol], "-u", up.url(paths[protocol]), "-m", "m",
                   "--chunk-timeout", "1") as ai:
            events = sse_of(answer)
            wire = sse_wire(events)
            for variant in (wire, wire.replace(b"\n", b"\r\n"), sse_wire(events, True),
                            wire.rstrip(b"\n")):
                up.reply = lambda request, w=variant: (200, Stream(wire=w, sizes=cuts))
                assert text_of(ai.ask()) == text, (protocol, variant[:100])
            up.plain = True
            up.reply = lambda request, p=protocol: (200, ANSWERS[p])
            assert text_of(ai.ask()) == "A", protocol
            # No text: the same stop reason, streamed or not.
            up.reply = lambda request, p=protocol: (200, empty[p])
            plain = ai.ask()
            up.plain = False
            streamed = ai.ask()
            assert streamed == plain and plain["isError"], (protocol, streamed, plain)

            # A stream that ends before it says the answer has: cut off.
            up.reply = lambda request: (200, Stream(wire=sse_wire(events[:3])))
            result = ai.ask()
            assert result["isError"] and text_of(result) == \
                "Upstream request failed: Transferred a partial file: the stream " \
                "ended before the answer did", (protocol, result)

            # An error event: the text so far gives way to its message.
            up.cancelled.clear()
            up.reply = lambda request, p=protocol: (200, Stream(
                wire=sse_wire(events[:3] + [errors[p]]), stall=3))
            result = ai.ask()
            assert result["isError"] and \
                text_of(result) == "Upstream error (HTTP 200): boom", result
            assert up.cancelled.wait(2), "the upstream did not see the hang-up"
            values = ai.metrics()
            assert values["askai_upstream_chunks_total"] >= 4 * (len(events) - 1), \
                values

    with Askai(binary, "-u", up.url(), "--chunk-timeout", "1", "-T", "2") as ai:
        wire = sse_wire(sse_of(answers["openai"]))
        # A pause past the chunk timeout ends the call, and the upstream sees
        # the hang-up; a stream that keeps coming does not.
        up.cancelled.clear()
        up.reply = lambda request: (200, Stream(wire=wire, sizes=(40,), stall=3))
        start = time.monotonic()
        result = ai.ask()
        assert result["isError"] and text_of(result) == \
            "Upstream answer stalled: nothing came for 1 s.", result
        assert time.monotonic() - start < 2.5
        assert up.cancelled.wait(2), "the upstream did not see the hang-up"
        assert ai.metrics()["askai_upstream_stalls_total"] == 1
        up.reply = lambda request: (200, Stream(wire=wire, sizes=(len(wire) // 4,) * 4,
                                                gap=0.4))
        assert text_of(ai.ask()) == text
        # The deadline counts from the start upstream, whatever comes.
        up.reply = lambda request: (200, Stream(wire=wire, sizes=(20,) * 200, gap=0.1))
        result = ai.ask()
        assert result["isError"] and "timed out after 2 s" in text_of(result), result
        # Text that is not UTF-8, which json-c lets through, ends the call;
        # so does an event that is not JSON text, with text that json-c
        # finds no UTF-8 or cut short.
        bad = b'data: {"choices":[{"delta":{"content":"\xc0\xa0"}}]}\n\n'
        up.reply = lambda request: (200, Stream(wire=bad + wire))
        result = ai.ask()
        assert result["isError"] and text_of(result) == \
            "Upstream error (HTTP 200): an answer that is not text", result
        for bad in (b'data: {"choices":[{"delta":{"content":"BAD\xff"}}]}\n\n',
                    b'data: {"choices":[{"delta":{"content":"LOST"}}\n\n'):
            up.reply = lambda request, b=bad: (200, Stream(
                wire=wire[:60] + b + wire[60:]))
            result = ai.ask()
            assert result["isError"] and text_of(result) == \
                "Upstream error (HTTP 200): an event that is not JSON text", result
        # The first completion's text, should --extra ask for more.
        events = [(None, {"choices": [{"index": i % 2, "delta": {"content": c}}]})
                  for i, c in enumerate("HwEoLrLlOd")]
        events += [(None, {"choices": [{"index": i, "delta": {},
                                        "finish_reason": "stop"}]})
                   for i in (1, 0)] + [(None, "[DONE]")]
        up.reply = lambda request: (200, Stream(wire=sse_wire(events)))
        assert text_of(ai.ask()) == "HELLO"
    up.reply = lambda request: (200, ANSWERS["openai"])

    print("streamed answers in every protocol, error events, stalls: PASS")


def check_scheduler(binary, up):
    """Home starts the jobs of the principals with jobs queued round-robin, a
    job each, as its slots free: a session's, or else a client address's.
    One principal may have eight queued beyond the slots free, and the next
    is refused at once as busy; a flooder's queued calls then delay another
    session's call by one service time at most, where one queue, first in
    first out, would hold it behind them all.  A call that goes while queued
    never reaches the upstream, whether its client hangs up or withdraws it;
    one that goes while its answer streams is cancelled upstream.  --timeout
    bounds a call's wait in its queue, and then its upstream call apart."""
    service = 0.3

    chunk = sse_wire([(None, {"choices": [{"index": 0, "delta": {"content": "x"}}]})])

    def reply(request):
        raw = request["raw"].decode()
        if "STREAM" in raw:
            return 200, Stream(wire=chunk * 200, sizes=(len(chunk),) * 200, gap=0.02)
        return 200, ANSWERS["openai"], 2.0 if "HOLD" in raw else \
            service if "SLOW" in raw else 0

    def finish(socks):
        for sock in socks:
            sock.settimeout(10)
            assert text_of(Replies(sock).next()[1]["result"]) == "A"
            sock.close()

    def contend(ai, fifo):
        """The flooder's nine calls, then another session's: its latency, and
        its place among the upstream's requests."""
        count = len(up.requests)
        socks = []
        for i in range(9):
            socks.append(socket.create_connection(("127.0.0.1", ai.port), 10))
            socks[-1].sendall(session_call(1, i, f"SLOW F{i}"))
        wait_for(lambda: ai.metrics()["askai_jobs_queued"] == 8)
        values = ai.metrics()
        assert values["askai_principals_queued"] == 1, values
        if not fifo:
            with socket.create_connection(("127.0.0.1", ai.port), 10) as sock:
                start = time.monotonic()
                sock.sendall(session_call(1, 99, "SLOW F99"))
                busy = Replies(sock).next()[1]["result"]
                assert busy["isError"] and "try again" in text_of(busy), busy
                assert time.monotonic() - start < 0.5
        with socket.create_connection(("127.0.0.1", ai.port), 10) as sock:
            start = time.monotonic()
            sock.sendall(session_call(2, 1, "SLOW I"))
            assert text_of(Replies(sock).next()[1]["result"]) == "A"
            took = time.monotonic() - start
        finish(socks)
        raws = [r["raw"].decode() for r in up.requests[count:]]
        return took, next(i for i, raw in enumerate(raws) if "SLOW I" in raw)

    up.reply = reply
    with Askai(binary, "-u", up.url(), "--slots", "1") as ai:
        took, place = contend(ai, False)
        assert place <= 2 and took < 3 * service + 0.4, (place, took)
        values = ai.metrics()
        assert values["askai_jobs_dispatched_total"] == 10, values
        assert values["askai_principals_queued"] == 0, values

        # Gone while queued: the upstream never sees the call.
        count = len(up.requests)
        hold = socket.create_connection(("127.0.0.1", ai.port), 10)
        hold.sendall(session_call(3, 1, "HOLD"))
        wait_for(lambda: len(up.requests) == count + 1)
        with socket.create_connection(("127.0.0.1", ai.port), 10) as sock:
            sock.sendall(session_call(4, 1, "GONE", Accept="text/event-stream"))
            assert b"text/event-stream" in sock.recv(4096)
            wait_for(lambda: ai.metrics()["askai_jobs_queued"] == 1)
        wait_for(lambda: ai.metrics()["askai_jobs_queued"] == 0)
        with socket.create_connection(("127.0.0.1", ai.port), 10) as sock:
            sock.sendall(session_call(5, 7, "WITHDRAWN", Accept="text/event-stream"))
            assert b"text/event-stream" in sock.recv(4096)
            wait_for(lambda: ai.metrics()["askai_jobs_queued"] == 1)
            assert ai.rpc("notifications/cancelled", {"requestId": 7}, rpc_id=None,
                          headers={"Mcp-Session-Id": f"{5:032x}",
                                   "MCP-Protocol-Version": LEGACY})[0] == 202
            sock.settimeout(5)
            rest = b""
            while not rest.endswith(b"0\r\n\r\n"):
                piece = sock.recv(4096)
                assert piece, rest
                rest += piece
            assert b"data:" not in rest, rest
        finish([hold])
        raws = [r["raw"].decode() for r in up.requests[count:]]
        assert not any("GONE" in raw or "WITHDRAWN" in raw for raw in raws), raws

        # Gone while its answer streams, in either way: cancelled upstream.
        for withdraw in (False, True):
            up.cancelled.clear()
            sock = socket.create_connection(("127.0.0.1", ai.port), 10)
            sock.sendall(session_call(6, 8, "STREAM", Accept="text/event-stream"))
            wait_for(lambda: ai.metrics()["askai_upstream_chunks_total"] >= 20 +
                     values["askai_upstream_chunks_total"])
            if withdraw:
                assert ai.rpc("notifications/cancelled", {"requestId": 8}, rpc_id=None,
                              headers={"Mcp-Session-Id": f"{6:032x}",
                                       "MCP-Protocol-Version": LEGACY})[0] == 202
            else:
                sock.close()
            assert up.cancelled.wait(3), "the stream was not cancelled upstream"
            sock.close()
            values = ai.metrics()
        assert values["askai_upstream_cancels_total"] >= 2, values

    # One queue for all, first in first out: the call waits for all nine.
    with Askai(binary, "-u", up.url(), "--slots", "1", "--test-fifo") as ai:
        took, place = contend(ai, True)
        assert place == 9 and took > 8 * service, (place, took)

    # Without a session, a call's principal is its client's address: one
    # address's tenth call is refused, while another's starts within a round.
    with Askai(binary, "-u", up.url(), "--slots", "1") as ai:
        count = len(up.requests)
        socks = []
        for i in range(10):
            socks.append(socket.create_connection(("127.0.0.1", ai.port), 10))
            socks[-1].sendall(rpc_request(i, {"name": "ask_ai", "arguments": {
                "prompt": f"SLOW A{i}"}}, "tools/call"))
            if i == 8:
                wait_for(lambda: ai.metrics()["askai_jobs_queued"] == 8)
        busy = Replies(socks.pop()).next()[1]["result"]
        assert busy["isError"] and "try again" in text_of(busy), busy
        with socket.create_connection(("127.0.0.1", ai.port), 10,
                                      source_address=("127.0.0.2", 0)) as sock:
            sock.sendall(rpc_request(99, {"name": "ask_ai", "arguments": {
                "prompt": "SLOW B"}}, "tools/call"))
            assert text_of(Replies(sock).next()[1]["result"]) == "A"
        finish(socks)
        raws = [r["raw"].decode() for r in up.requests[count:]]
        assert next(i for i, raw in enumerate(raws) if "SLOW B" in raw) <= 2, raws

    # --timeout counts a call's wait in the queue from its submission, and
    # its upstream call from its start: the second call waits 0.8 s and is
    # answered 0.7 s later, while the third's second in the queue runs out.
    up.reply = lambda request: (200, ANSWERS["openai"],
                                0.8 if "FIRST" in request["raw"].decode() else 0.7)
    with Askai(binary, "-u", up.url(), "--slots", "1", "-T", "1") as ai:
        count = len(up.requests)
        socks = []
        for i, prompt in enumerate(("FIRST", "SECOND", "THIRD")):
            socks.append(socket.create_connection(("127.0.0.1", ai.port), 10))
            socks[-1].sendall(session_call(i, i, prompt))
            if not i:
                wait_for(lambda: len(up.requests) == count + 1)
        results = [Replies(sock).next()[1]["result"] for sock in socks]
        for sock in socks:
            sock.close()
        assert [text_of(result) for result in results] == [
            "A", "A", "Timed out after 1 s waiting for an upstream slot."], results
        values = ai.metrics()
        assert values['askai_jobs_ended_total{end="rejected"}'] == 1, values
        assert len(up.requests) == count + 2
    up.reply = lambda request: (200, ANSWERS["openai"])

    print("scheduler: principals' queues, round-robin, cancelled while queued "
          "or streaming: PASS")


def check_breaker(binary, up):
    """Five upstream failures in a row open the breaker: tool calls are then
    refused at once, saying when to try again, and those queued end so too;
    after a backoff of a second, doubling to a minute, one probe goes, alone,
    whose failure opens it again and whose success, as soon as its answer
    begins, closes it.  An answer's Retry-After opens it at once, for as
    long."""
    def failing(delay=0):
        return lambda request: (503, {"error": {"message": "down"}}, delay)

    up.reply = failing()
    with Askai(binary, "-u", up.url()) as ai:
        for _ in range(5):
            result = ai.ask()
            assert text_of(result) == "Upstream error (HTTP 503): down", result
        count = len(up.requests)
        result = ai.ask()
        assert result["isError"] and text_of(result) == \
            "The upstream is unavailable; try again in 1 s.", result
        assert len(up.requests) == count
        values = ai.metrics()
        assert (values["askai_breaker_state"], values["askai_breaker_opens_total"]) \
            == (1, 1), values
        ai.wait_log(r"upstream unavailable: tool calls refused for 1 s")
        time.sleep(1.1)
        # Half-open: the probe fails, and the breaker opens for two seconds.
        assert text_of(ai.ask()) == "Upstream error (HTTP 503): down"
        assert len(up.requests) == count + 1
        assert text_of(ai.ask()) == "The upstream is unavailable; try again in 2 s."
        time.sleep(2.1)
        up.reply = lambda request: (200, ANSWERS["openai"])
        assert text_of(ai.ask()) == "A"
        values = ai.metrics()
        assert (values["askai_breaker_state"], values["askai_breaker_opens_total"],
                values["askai_breaker_probes_total"]) == (0, 2, 2), values
        ai.wait_log(r"upstream breaker closed")

        # Retry-After opens it at once.
        up.reply = lambda request: (429, {"error": {"message": "slow down"}})
        up.headers = {"Retry-After": "3"}
        assert text_of(ai.ask()) == "Upstream error (HTTP 429): slow down"
        assert text_of(ai.ask()) == "The upstream is unavailable; try again in 3 s."
        up.headers = {}

    # The calls queued when it opens end at once.
    up.reply = failing(0.1)
    with Askai(binary, "-u", up.url(), "--slots", "1") as ai:
        results = []
        threads = [threading.Thread(target=lambda i=i: results.append(ai.rpc(
            "tools/call", {"name": "ask_ai", "arguments": {"prompt": "Q"}},
            headers={"Mcp-Session-Id": f"{i:032x}"})[2]["result"]))
            for i in range(7)]
        for thread in threads:
            thread.start()
            time.sleep(0.02)
        for thread in threads:
            thread.join(10)
        texts = sorted(text_of(result) for result in results)
        assert texts == ["The upstream is unavailable; try again in 1 s."] * 2 + \
            ["Upstream error (HTTP 503): down"] * 5, texts

    # Half-open, exactly one probe goes; the calls queued meanwhile, eight a
    # session though slots are free, wait for it.  Its failure ends them;
    # its streamed answer, once begun, starts them, however long it goes on.
    chunk = sse_wire([(None, {"choices": [{"index": 0, "delta": {"content": "x"}}]})])
    probe = Stream(wire=chunk * 30 + sse_wire([(None, "[DONE]")]),
                   sizes=(len(chunk),) * 30, gap=0.1)
    state = {"fail": True}

    def reply(request):
        probing = "PROBE" in request["raw"].decode()
        if state["fail"]:
            return 503, {"error": {"message": "down"}}, 0.8 if probing else 0
        return (200, probe if probing else ANSWERS["openai"])

    up.reply = reply
    with Askai(binary, "-u", up.url(), "--slots", "4") as ai:
        for _ in range(5):
            ai.ask()
        time.sleep(1.1)
        count = len(up.requests)
        first = socket.create_connection(("127.0.0.1", ai.port), 10)
        first.sendall(session_call(1, 1, "PROBE"))
        wait_for(lambda: len(up.requests) == count + 1)
        socks = []
        for i in range(10):
            socks.append(socket.create_connection(("127.0.0.1", ai.port), 10))
            socks[-1].sendall(session_call(2, i))
        wait_for(lambda: ai.metrics()["askai_jobs_queued"] == 8)
        texts = [text_of(Replies(sock).next()[1]["result"]) for sock in [first] + socks]
        assert texts[0] == "Upstream error (HTTP 503): down", texts
        assert sorted(texts[1:]) == \
            ["The upstream is unavailable; try again in 2 s."] * 8 + \
            ["Too many tool calls are under way; try again in a few seconds."] * 2, texts
        assert len(up.requests) == count + 1
        for sock in [first] + socks:
            sock.close()

        state["fail"] = False
        time.sleep(2.1)
        first = socket.create_connection(("127.0.0.1", ai.port), 10)
        first.sendall(session_call(3, 1, "PROBE"))
        wait_for(lambda: ai.metrics()["askai_breaker_state"] == 0)
        start = time.monotonic()
        socks = []
        for i in range(3):
            socks.append(socket.create_connection(("127.0.0.1", ai.port), 10))
            socks[-1].sendall(session_call(4 + i, i))
        texts = [text_of(Replies(sock).next()[1]["result"]) for sock in socks]
        assert texts == ["A"] * 3, texts
        assert time.monotonic() - start < 1.5
        assert text_of(Replies(first).next()[1]["result"]) == "x" * 30
        for sock in [first] + socks:
            sock.close()
        values = ai.metrics()
        assert (values["askai_breaker_opens_total"],
                values["askai_breaker_probes_total"]) == (2, 2), values
    up.reply = lambda request: (200, ANSWERS["openai"])

    print("breaker: open after failures, refusals, probes, Retry-After: PASS")


def check_slots(binary, up):
    """Home asks llama-server's /props for its slots, the upstream calls it
    makes at once, in any protocol llama-server speaks; --slots says so
    instead, and without an answer, in time or at all, or for Gemini's
    protocol, it makes four."""
    up.props = {"total_slots": 2, "chat_template": "{{x}}" * 4000,
                "default_generation_settings": {"n_ctx": 4096}}
    up.reply = lambda request: (200, ANSWERS["openai"],
                                0.5 if "SLOW" in request["raw"].decode() else 0)
    with Askai(binary, "-u", up.url()) as ai:
        ai.wait_log(r"upstream slots: 2 \(llama-server's /props\)")
        assert ai.metrics()["askai_upstream_slots"] == 2
        count = len(up.requests)
        socks = []
        for i in range(3):
            socks.append(socket.create_connection(("127.0.0.1", ai.port), 10))
            socks[-1].sendall(session_call(i, i, "SLOW"))
        wait_for(lambda: len(up.requests) == count + 2)
        time.sleep(0.2)
        assert len(up.requests) == count + 2
        for sock in socks:
            assert text_of(Replies(sock).next()[1]["result"]) == "A"
            sock.close()
    for props, why in (({"total_slots": "2"}, r"/props gives no total_slots"),
                       (None, r"/props: HTTP 404")):
        up.props = props
        with Askai(binary, "-u", up.url()) as ai:
            ai.wait_log(r"upstream slots: 4 \(the default: " + why + r"\)")
    gets = up.gets
    with Askai(binary, "-u", up.url(), "--slots", "3") as ai:
        ai.wait_log(r"upstream slots: 3 \(--slots\)")
    # Only a server that may be llama-server is asked: one that no provider
    # but llama names, and that has no Gemini API.
    for args in (("--api", "generate", "-u", up.url("/v1beta/models/m")),
                 ("-p", "openai", "-u", up.url()),
                 ("-p", "anthropic", "-u", up.url("/v1/messages"))):
        with Askai(binary, *args) as ai:
            ai.wait_log(r"upstream slots: 4 \(the default\)")
    assert up.gets == gets
    up.props = {"total_slots": 2}
    for args in (("--api", "messages", "-u", up.url("/v1/messages")),
                 ("-p", "llama", "-u", up.url()),
                 ("-p", "llama", "--api", "responses", "-u", up.url("/v1/responses"))):
        with Askai(binary, *args) as ai:
            ai.wait_log(r"upstream slots: 2 \(llama-server's /props\)")
    # /props may take the calls' timeout at most.
    up.props = "hang"
    with Askai(binary, "-u", up.url(), "-T", "1") as ai:
        ai.wait_log(r"upstream slots: 4 \(the default: /props: Operation timed out")
    up.props = None
    up.reply = lambda request: (200, ANSWERS["openai"])

    print("upstream slots: from /props, --slots, or the default: PASS")


def read_events(sock, timeout=10):
    """The events of an event stream read off @p sock to its end, each with
    when it came."""
    sock.settimeout(timeout)
    raw = b""
    seen = 0
    events = []
    while not raw.endswith(b"\r\n0\r\n\r\n"):
        chunk = sock.recv(65536)
        assert chunk, raw
        raw += chunk
        found = re.findall(rb"data: (.*?)\n\n", raw[raw.index(b"\r\n\r\n"):])
        events += [(time.monotonic(), json.loads(e)) for e in found[seen:]]
        seen = len(found)
    return events


def check_progress(binary, up):
    """A call whose request has a progressToken gets notifications/progress on
    its event stream, at most one every 100 ms, their progress growing: its
    place in its session's queue, then the tokens of its answer, "thinking"
    while the latest is the model's thinking, in every protocol.  None comes
    after the result, or once the call is cancelled, nor to a call without a
    token, nor in a JSON reply."""
    chunk = sse_wire([(None, {"choices": [{"index": 0, "delta": {"content": "x"}}]})])
    wire = chunk * 30 + sse_wire([(None, "[DONE]")])

    def reply(request):
        raw = request["raw"].decode()
        if "SLOW" in raw:
            return 200, ANSWERS["openai"], 0.6
        return 200, Stream(wire=wire, sizes=(len(chunk),) * 30, gap=0.05)

    def call(i, rpc_id, token=None, accept="application/json, text/event-stream"):
        params = {"name": "ask_ai", "arguments": {"prompt": "Q"}}
        if token is not None:
            params["_meta"] = {"progressToken": token}
        return rpc_request(rpc_id, params, "tools/call",
                           headers={"Mcp-Session-Id": f"{i:032x}",
                                    "MCP-Protocol-Version": LEGACY, "Accept": accept})

    up.reply = reply
    with Askai(binary, "-u", up.url(), "--slots", "1") as ai:
        count = len(up.requests)
        slow = socket.create_connection(("127.0.0.1", ai.port), 10)
        slow.sendall(session_call(1, 1, "SLOW"))
        wait_for(lambda: len(up.requests) == count + 1)
        with socket.create_connection(("127.0.0.1", ai.port), 10) as sock:
            sock.sendall(call(2, 5, "tok"))
            events = read_events(sock)
        notes = [(t, e["params"]) for t, e in events
                 if e.get("method") == "notifications/progress"]
        assert events[-1][1]["id"] == 5 and text_of(events[-1][1]["result"]) == "x" * 30
        assert all("method" in e for _, e in events[:-1]), events
        assert notes[0][1]["message"] == "queued, position 1", notes
        assert all(n["progressToken"] == "tok" for _, n in notes), notes
        progress = [n["progress"] for _, n in notes]
        assert progress == sorted(set(progress)) and progress[0] == 0, progress
        assert re.fullmatch(r"generating, \d+ tokens", notes[-1][1]["message"]), notes
        times = [t for t, _ in notes]
        assert len(notes) <= (times[-1] - times[0]) / 0.1 + 2, times
        assert text_of(Replies(slow).next()[1]["result"]) == "A"
        slow.close()

        # Without a token, or in a JSON reply, none comes; a numeric token
        # is repeated as a number.
        with socket.create_connection(("127.0.0.1", ai.port), 10) as sock:
            sock.sendall(call(3, 6))
            events = read_events(sock)
            assert len(events) == 1 and "result" in events[0][1], events
        with socket.create_connection(("127.0.0.1", ai.port), 10) as sock:
            sock.sendall(call(3, 7, "tok", "application/json"))
            assert text_of(Replies(sock).next()[1]["result"]) == "x" * 30
        with socket.create_connection(("127.0.0.1", ai.port), 10) as sock:
            sock.sendall(call(3, 8, 42))
            events = read_events(sock)
            assert events[0][1]["params"]["progressToken"] == 42, events

        # Withdrawn as its answer streams: the stream ends, no result.
        with socket.create_connection(("127.0.0.1", ai.port), 10) as sock:
            sock.sendall(call(4, 9, "tok"))
            wait_for(lambda: ai.metrics()["askai_upstream_calls"] == 1)
            time.sleep(0.3)
            assert ai.rpc("notifications/cancelled", {"requestId": 9}, rpc_id=None,
                          headers={"Mcp-Session-Id": f"{4:032x}",
                                   "MCP-Protocol-Version": LEGACY})[0] == 202
            events = read_events(sock)
            assert events and all(e.get("method") == "notifications/progress"
                                  for _, e in events), events

    # Thinking counts, said so while the latest token is; the answer is the
    # text alone.
    thinking = {
        "openai": {"choices": [{"message": {"reasoning_content": "hmm",
                                            "content": "xyz"},
                                "finish_reason": "stop"}]},
        "responses": {"status": "completed", "output": [
            {"type": "reasoning", "content": [{"type": "reasoning_text", "text": "hmm"}]},
            {"type": "message", "content": [{"type": "output_text", "text": "xyz"}]}]},
        "anthropic": {"content": [{"type": "thinking", "thinking": "hmm"},
                                  {"type": "text", "text": "xyz"}],
                      "stop_reason": "end_turn"},
        "gemini": {"candidates": [{"content": {"parts": [
            {"text": "hmm", "thought": True}, {"text": "xyz"}]},
            "finishReason": "STOP"}]},
    }
    paths = {"openai": "/v1/chat/completions", "responses": "/v1/responses",
             "anthropic": "/v1/messages", "gemini": "/v1beta/models/m:generateContent"}
    for protocol, answer in thinking.items():
        events = sse_of(answer)
        sizes = tuple(len(sse_wire([e])) for e in events)
        up.reply = lambda request, w=sse_wire(events), s=sizes: (
            200, Stream(wire=w, sizes=s, gap=0.15))
        with Askai(binary, "--api", API[protocol], "-u", up.url(paths[protocol]), "-m", "m",
                   "--slots", "1") as ai:
            with socket.create_connection(("127.0.0.1", ai.port), 10) as sock:
                sock.sendall(call(5, 10, "tok"))
                events = read_events(sock)
        assert text_of(events[-1][1]["result"]) == "xyz", (protocol, events)
        notes = [e["params"] for _, e in events[:-1]]
        progress = [n["progress"] for n in notes]
        assert progress == sorted(set(progress)), (protocol, notes)
        said = [re.fullmatch(r"(thinking|generating), (\d+) tokens", n["message"])
                for n in notes]
        kinds = [m[1] for m in said if m and int(m[2])]
        assert kinds and kinds[0] == "thinking" and kinds[-1] == "generating" and \
            kinds == sorted(kinds, key=lambda k: k == "generating"), (protocol, notes)
    up.reply = lambda request: (200, ANSWERS["openai"])

    print("progress: notifications on the event stream, rate-limited: PASS")


def check_jobs(binary, up):
    """Home runs the upstream calls as jobs (design section 4).  A call past
    --max-calls is answered at once as busy, a tool error that says to try
    again.  A call whose client goes while it waits cancels its job and frees
    its handle, so the job's completion, however it races the hang-up, finds
    no call and is dropped, and a call that takes the slot next gets its own
    answer.  Every job ends once: submitted = completed + cancelled +
    rejected, with none left in flight, and the completion of every call
    cancelled while it waited is dropped.  /metrics shows home's loop and
    stash, and the jobs.  Shutting down cancels the jobs in flight."""
    call = {"name": "ask_ai", "arguments": {"prompt": "Q"}}
    def ended(values, end):
        return values[f'askai_jobs_ended_total{{end="{end}"}}']

    def cancelled(values):
        return total(values, "askai_calls_cancelled_total")

    def dropped(values):
        return total(values, "askai_calls_dropped_total")

    def settled(ai):
        """The metrics once no job is in flight and each cancelled call's
        completion has come back to its loop, which must account for every
        job."""
        for _ in range(250):
            values = ai.metrics()
            if not values["askai_jobs_in_flight"] and \
                    cancelled(values) == dropped(values):
                break
            time.sleep(0.02)
        assert values["askai_jobs_submitted_total"] == sum(
            ended(values, end) for end in ("completed", "cancelled", "rejected")), \
            values
        assert not values["askai_jobs_in_flight"], values
        assert cancelled(values) == dropped(values), values
        return values

    with Askai(binary, "-u", up.url(), "--max-calls", "2") as ai:
        assert text_of(ai.ask()) == "A"
        values = settled(ai)
        for series, expected in (("askai_jobs_submitted_total", 1),
                                 ('askai_jobs_ended_total{end="completed"}', 1)):
            assert values[series] == expected, (series, values)
        assert total(values, "askai_spare_jobs") == 1, values
        for series in ('askai_loop_turn_seconds_count{loop="home"}',
                       'askai_loop_step_seconds_count{loop="home",kind="bulk"}',
                       'askai_loop_inbox_delay_seconds_count{loop="home"}',
                       'askai_stash_takes_total{loop="home",result="miss"}',
                       "askai_upstream_run_seconds_count"):
            assert values[series] >= 1, (series, values)
        # The call's loop had its completion.
        assert total(values, "askai_loop_inbox_delay_seconds_count") > \
            values['askai_loop_inbox_delay_seconds_count{loop="home"}'], values
        assert 'askai_spare_jobs{loop="home"}' not in values

        # Busy: with two calls under way, a third is refused at once, in
        # either era.
        up.reply = lambda request: (200, ANSWERS["openai"], 1.5)
        count = len(up.requests)
        results = []
        threads = [threading.Thread(target=lambda: results.append(ai.ask("SLOW")))
                   for _ in range(2)]
        for thread in threads:
            thread.start()
        wait_for(lambda: len(up.requests) == count + 2)
        start = time.monotonic()
        result = ai.ask()
        _, headers, body = ai.modern("tools/call", call)
        assert time.monotonic() - start < 1, "a busy call waited"
        for busy in (result, body["result"]):
            assert busy["isError"] is True and "try again" in text_of(busy), busy
        assert headers["content-type"] == "text/event-stream"
        for thread in threads:
            thread.join(5)
        assert [text_of(result) for result in results] == ["A", "A"], results
        up.reply = lambda request: (200, ANSWERS["openai"])
        assert text_of(ai.ask()) == "A"
        assert len(up.requests) == count + 3
        values = settled(ai)
        assert ended(values, "rejected") == 2, values
        assert ended(values, "completed") == 4, values

    def reply(request):
        raw = request["raw"].decode()
        jitter = re.search(r"JITTER (\d+)", raw)
        delay = 5 if "SLOW" in raw else 0.3 if "RACE" in raw else \
            0.02 + 0.01 * (int(jitter.group(1)) % 5) if jitter else 0
        return 200, {"choices": [{"message": {"content": "B" if "B" in raw else "A"},
                                  "finish_reason": "stop"}]}, delay

    up.reply = reply
    with Askai(binary, "-u", up.url(), "--slots", "64") as ai:
        # A client that goes while its call waits: the job is cancelled
        # upstream, and its completion dropped.
        hangups = up.hangups
        with socket.create_connection(("127.0.0.1", ai.port), 10) as sock:
            sock.sendall(rpc_request(1, {"name": "ask_ai", "arguments": {
                "prompt": "SLOW"}}, "tools/call", accept_sse=True))
            assert b"Content-Type: text/event-stream" in sock.recv(4096)
            wait_for(lambda: len(up.requests) and "SLOW" in up.last()["raw"].decode())
        wait_for(lambda: up.hangups > hangups)
        values = settled(ai)
        assert (cancelled(values), dropped(values), ended(values, "cancelled")) == \
            (1, 1, 1), values

        # The answer comes as the client goes, askai stopped meanwhile: home
        # either finishes the job or cancels it, and the completion finds the
        # handle stale; the call that takes the slot next gets its own answer.
        # (The completion comes first here: tests/handle.c names the slot
        # again before a stale handle's lookup.)
        for i in range(3):
            count = len(up.requests)
            sock = socket.create_connection(("127.0.0.1", ai.port), 10)
            sock.sendall(rpc_request(i, {"name": "ask_ai", "arguments": {
                "prompt": "RACE"}}, "tools/call", accept_sse=True))
            assert b"Content-Type: text/event-stream" in sock.recv(4096)
            wait_for(lambda: len(up.requests) > count)
            ai.proc.send_signal(signal.SIGSTOP)
            try:
                time.sleep(0.5)
                sock.close()
            finally:
                ai.proc.send_signal(signal.SIGCONT)
            assert text_of(ai.ask("B")) == "B"
        settled(ai)

        # Many at once, each hanging up about when its answer comes: one
        # address's, which slots free take at once, none refused.
        rejected = ended(ai.metrics(), "rejected")

        def racer(i):
            with socket.create_connection(("127.0.0.1", ai.port), 10) as sock:
                sock.sendall(rpc_request(i, {"name": "ask_ai", "arguments": {
                    "prompt": f"JITTER {i}"}}, "tools/call", accept_sse=True))
                sock.recv(4096)
                time.sleep(0.01 * (i % 8))

        threads = [threading.Thread(target=racer, args=(i,)) for i in range(64)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join(10)
        values = settled(ai)
        assert cancelled(values) > 1 and values["askai_jobs_submitted_total"] >= 69, \
            values
        assert ended(values, "rejected") == rejected, values
        assert text_of(ai.ask("B")) == "B"

    # Shutting down with jobs in flight: each call ends with the error, in
    # either kind of reply, each job running is cancelled upstream and none
    # queued starts, and home's count says so.  The calls are sessions' of
    # their own, half of them with a slot each.
    hangups = up.hangups
    count = len(up.requests)
    ai = Askai(binary, "-u", up.url(), "--slots", "8")
    try:
        socks = []
        for i in range(16):
            socks.append(socket.create_connection(("127.0.0.1", ai.port), 10))
            socks[-1].sendall(session_call(
                i, i, "SLOW", Accept="application/json" +
                (", text/event-stream" if i % 2 == 0 else "")))
        wait_for(lambda: len(up.requests) == count + 8 and
                 ai.metrics()["askai_jobs_queued"] == 8)
        ai.proc.send_signal(signal.SIGTERM)
        for i, sock in enumerate(socks):
            sock.settimeout(5)
            rest = b""
            while chunk := sock.recv(65536):
                rest += chunk
            assert b'"id":%d,"error":{"code":-32603,"message":"Server is shutting ' \
                   b'down"}' % i in rest, rest
            sock.close()
        assert ai.proc.wait(5) == 0, "".join(ai.log)
    finally:
        if ai.proc.poll() is None:
            ai.proc.kill()
            ai.proc.wait()
    wait_for(lambda: up.hangups == hangups + 8)
    assert len(up.requests) == count + 8
    ai.wait_log(r"jobs: 16 submitted, 0 completed, 16 cancelled, 0 rejected; "
                r"8 cancelled upstream; 0 preempted$")
    up.reply = lambda request: (200, ANSWERS["openai"])

    print("jobs: busy, stale handles, cancels racing completions, shutdown: PASS")


def check_watchdog(binary, up):
    """The watchdog reports a turn that goes on too long, once, of either
    loop, the client loop or home; a shutdown signal ends a stuck loop's
    process at once, as does a second signal."""
    with Askai(binary, "-u", up.url(), "--test-stall", "300") as ai:
        conn = ai.connect()
        assert ai.rpc("ping", conn=conn)[0] == 200
        ai.proc.send_signal(signal.SIGUSR2)
        time.sleep(0.05)
        start = time.monotonic()
        assert ai.rpc("ping", conn=conn)[0] == 200
        assert time.monotonic() - start > 0.15, "the loop did not stall"
        conn.close()
        for name in ("loop1", "home"):
            ai.wait_log(r"watchdog: the event loop has been in one turn for \d+.* "
                        rf"\({name}\)$")
        values = ai.metrics()
        assert values["askai_watchdog_reports_total"] >= 2, values
        assert values['askai_loop_turn_seconds_bucket{loop="home",le="0.25"}'] < \
            values['askai_loop_turn_seconds_count{loop="home"}']
        assert values['askai_loop_turn_seconds_bucket{loop="1",le="0.25"}'] < \
            values['askai_loop_turn_seconds_count{loop="1"}']

    # The limits allow for a sanitizer's own exit: TSan sleeps for 1 s.
    for signals, why, limit in (((signal.SIGINT,), "the event loop is stuck|"
                                 "the grace period is over", 3 + 1.5),
                                ((signal.SIGTERM, signal.SIGTERM), "a second signal",
                                 1.5)):
        ai = Askai(binary, "-u", up.url(), "--test-stall", "60000", "--grace", "3")
        try:
            ai.proc.send_signal(signal.SIGUSR2)
            time.sleep(0.2)
            for sig in signals:
                start = time.monotonic()
                ai.proc.send_signal(sig)
                time.sleep(0.05)
            assert ai.proc.wait(10) == 3, "".join(ai.log)
            assert time.monotonic() - start < limit, time.monotonic() - start
            ai.wait_log("shutting down at once: (" + why + ")")
        finally:
            if ai.proc.poll() is None:
                ai.proc.kill()
                ai.proc.wait()

    # A second signal just behind the first: the loop may finish its drain
    # first, or the process ends at once.
    ai = Askai(binary, "-u", up.url())
    ai.proc.send_signal(signal.SIGTERM)
    ai.proc.send_signal(signal.SIGINT)
    assert ai.proc.wait(5) in (0, 3), "".join(ai.log)

    print("watchdog: stalls reported, stuck loops and second signals: PASS")


def check_stalled_stderr(binary, up):
    """A reader of askai's stderr that never reads holds up no client: the
    logger alone writes stderr, and blocks; lines are dropped and counted,
    and shutdown does not wait for them."""
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    read_end, write_end = os.pipe()
    fcntl.fcntl(write_end, 1031, 4096)  # F_SETPIPE_SZ
    os.set_blocking(write_end, False)
    try:
        while os.write(write_end, b"x" * 4096):
            pass
    except BlockingIOError:
        pass
    os.set_blocking(write_end, True)
    env = {k: v for k, v in os.environ.items() if k not in KEY_VARS}
    proc = subprocess.Popen([binary, "-l", f"127.0.0.1:{port}", "-u", up.url(), "-v"],
                            env=env, stdin=subprocess.DEVNULL,
                            stdout=subprocess.DEVNULL, stderr=write_end)
    os.close(write_end)
    try:
        for _ in range(500):
            with socket.socket() as sock:
                if not sock.connect_ex(("127.0.0.1", port)):
                    break
            time.sleep(0.01)
        conn = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
        ping = json.dumps({"jsonrpc": "2.0", "id": 1, "method": "ping"})
        headers = {"Content-Type": "application/json"}
        waits = []
        dropped = 0
        for i in range(4000):
            # Each SIGUSR1 has the loop log five lines of statistics.
            proc.send_signal(signal.SIGUSR1)
            time.sleep(0.001)
            if i % 25:
                continue
            start = time.monotonic()
            conn.request("POST", "/mcp", body=ping, headers=headers)
            response = conn.getresponse()
            response.read()
            waits.append(time.monotonic() - start)
            assert response.status == 200
            conn.request("GET", "/metrics")
            response = conn.getresponse()
            dropped = prometheus(response.read().decode())["askai_log_dropped_total"]
            if dropped > 100:
                break
        conn.close()
        assert dropped > 100, dropped
        assert max(waits) < 1, max(waits)
        start = time.monotonic()
        proc.send_signal(signal.SIGTERM)
        assert proc.wait(5) == 0
        assert time.monotonic() - start < 3
    finally:
        os.close(read_end)
        if proc.poll() is None:
            proc.kill()
            proc.wait()

    print("stalled stderr: lines dropped and counted, clients served: PASS")


# A stand-in for man(1).  With -w it finds a page's file in its directory's
# "pages", making it if need be, and notes its arguments in "finds"; else it
# renders the page that file names, noting its arguments in "renders", and
# the name says what it does.  It notes the processes that wait in
# "waiting".
FAKE_MAN = r"""#!/bin/sh
dir=${0%/*}
for page; do :; done
if [ "$1" = -w ]; then
	echo "$*" >> "$dir/finds"
	case $page in
	missing) echo "No manual entry for missing" >&2; exit 16 ;;
	fail) exit 3 ;;
	nowhere) echo pages/nowhere; exit 0 ;;
	shadow) [ -e "$dir/moved" ] && page=shadowed ;;
	esac
	[ -e "$dir/pages/$page" ] || : > "$dir/pages/$page"
	echo "$dir/pages/$page"
	exit 0
fi
echo "$*" >> "$dir/renders"
case ${page##*/} in
echo)
	printf '%s\n' "$@" ---
	env | sort
	echo ---
	ls /proc/self/fd | tr '\n' ' '
	echo
	readlink /proc/$$/fd/0 /proc/$$/fd/2
	grep -E '^Sig(Blk|Ign)' /proc/$$/status
	set -- $(cat /proc/$$/stat)
	echo "session $6 of $1"
	;;
broken) exit 3 ;;
big) head -c 3145728 /dev/zero | tr '\0' x ;;
endless) yes ;;
bytes) printf 'a\377b\303(c\342\202\254' ;;
slow) sleep 1; echo slow ;;
shadow) echo first ;;
shadowed) echo second ;;
wait*)
	sleep 1000 &
	echo "$$ $!" >> "$dir/waiting"
	wait
	;;
*) echo "page ${page##*/}" ;;
esac
"""


def man_call(ai, **arguments):
    status, _, body = ai.rpc("tools/call", {"name": "man", "arguments": arguments})
    assert status == 200, (status, body)
    return body["result"]


def alive(pid):
    """Whether process @p pid exists and is no zombie."""
    try:
        with open(f"/proc/{pid}/stat") as f:
            return f.read().rpartition(")")[2].split()[0] != "Z"
    except (FileNotFoundError, ProcessLookupError):
        return False


def check_man(binary, up):
    """The man tool, with --man only: its arguments checked; man run to find
    a page's file, then to render it, with an environment, signals, a session
    and descriptors of its own; the page cut at 2 MiB and mended into UTF-8
    text, or why there is none, which a turn's call is given whole, whatever
    its offset.  Each page is rendered once and kept by its file, for every
    name it is found by, eight at most, and given while its file is
    unchanged; a name is found anew, in the background, once the
    recheck has passed; the asks for a page under way wait for its one fetch.
    16 children at most, each killed with its group at its deadline or as
    askai stops; and the real man's pages, where they are installed."""
    with Askai(binary, "-u", up.url()) as ai:
        tools = ai.rpc("tools/list")[2]["result"]["tools"]
        assert [t["name"] for t in tools] == ["ask_ai"], tools
        _, _, body = ai.rpc("tools/call", {"name": "man", "arguments": {"name": "ls"}})
        assert body["error"]["message"] == "Unknown tool: man", body
    proc = subprocess.run([binary, "--man", "--tool-name", "man", "-u", up.url()],
                          capture_output=True, text=True, timeout=10)
    assert proc.returncode == 2, proc
    assert "--tool-name: askai has another tool named man" in proc.stderr, \
        proc.stderr

    with tempfile.TemporaryDirectory() as fake:
        with open(f"{fake}/man", "w") as f:
            f.write(FAKE_MAN)
        os.chmod(f"{fake}/man", 0o755)
        os.mkdir(f"{fake}/pages")
        path = fake + ":" + os.environ.get("PATH", "/usr/bin:/bin")
        env = {"PATH": path, "MANPATH": "/nowhere/man", "LANG": "fi_FI.UTF-8",
               "MANOPT": "-a", "MANPAGER": "less"}

        def lines(name):
            """The lines of the fake's file @p name."""
            try:
                with open(f"{fake}/{name}") as f:
                    return f.read().splitlines()
            except FileNotFoundError:
                return []

        def waiters():
            """The processes waiting, a line of the shell's and its sleep's
            for each."""
            return [[int(p) for p in line.split()] for line in lines("waiting")]

        def runs(page):
            """How often man found @p page, and rendered it."""
            return (sum(line.endswith(" -- " + page) for line in lines("finds")),
                    sum(line.endswith("/pages/" + page) for line in lines("renders")))

        with Askai(binary, "-u", up.url(), "--man", env=env) as ai:
            tools = ai.rpc("tools/list")[2]["result"]["tools"]
            assert [t["name"] for t in tools] == ["ask_ai", "man"], tools
            assert tools[1]["inputSchema"]["required"] == ["name"], tools[1]

            # Its arguments, environment, descriptors, signals and session.
            page = text_of(man_call(ai, name="echo", section="3p"))
            assert lines("finds") == ["-w -s 3p -- echo"], lines("finds")
            out = page.split("\n")
            args = out[:out.index("---")]
            assert args == ["-l", "--", f"{fake}/pages/echo"], out
            rest = out[len(args) + 1:]
            environ = [v for v in rest[:rest.index("---")]
                       if v.split("=")[0] not in ("PWD", "OLDPWD", "SHLVL", "_")]
            assert environ == ["LC_ALL=C.UTF-8", "MANPATH=/nowhere/man",
                               "MANROFFOPT=-rHY=0 -dAD=l", "MANWIDTH=80",
                               "PATH=" + path], environ
            fds, stdin, stderr, blocked, ignored, session = \
                rest[rest.index("---") + 1:-1]
            assert fds == "0 1 2 3 ", fds
            assert (stdin, stderr) == ("/dev/null", "/dev/null"), out
            assert blocked == "SigBlk:\t0000000000000000", out
            assert ignored == "SigIgn:\t0000000000000000", out
            sid, pid = re.fullmatch(r"session (\d+) of (\d+)", session).groups()
            assert sid == pid, session

            # Each name man finds the file by shares the one rendering, and
            # once found, a name's page comes from the manual.
            for section, find in ((3, "-w -s 3 -- echo"), ("", "-w -- echo")):
                assert text_of(man_call(ai, name="echo", section=section)) == page
                assert lines("finds")[-1] == find, (section, lines("finds"))
            for section in ("3p", 3, "", None):
                arguments = {"name": "echo", "section": section}
                if section is None:
                    del arguments["section"]
                assert text_of(man_call(ai, **arguments)) == page, section
            _, _, body = ai.modern("tools/call", {"name": "man",
                                                  "arguments": {"name": "echo"}})
            assert text_of(body["result"]) == page, body
            assert runs("echo") == (3, 1), runs("echo")
            values = ai.metrics()
            assert values["askai_man_asks_total{result=\"hit\"}"] == 5, values
            assert values["askai_man_asks_total{result=\"wait\"}"] == 3, values
            assert values["askai_man_runs_total{run=\"find\"}"] == 3, values
            assert values["askai_man_runs_total{run=\"render\"}"] == 1, values
            assert values["askai_man_pages"] == 1, values
            assert values["askai_man_page_bytes"] == len(page.encode()), values

            # A file that changes is rendered anew, as is one that goes: its
            # new rendering tells another session.
            os.chmod(f"{fake}/pages/echo", 0o600)
            again = text_of(man_call(ai, name="echo"))
            assert again != page and again.startswith(f"-l\n--\n{fake}/pages/echo\n")
            assert runs("echo") == (4, 2), runs("echo")
            os.unlink(f"{fake}/pages/echo")
            page = text_of(man_call(ai, name="echo", section=3))
            assert page != again and page.startswith(f"-l\n--\n{fake}/pages/echo\n")
            assert runs("echo") == (5, 3), runs("echo")
            assert ai.metrics()["askai_man_pages"] == 1

            # A file is kept by eight names at most: a ninth is found anew
            # each time, its page still from the manual.
            for i in range(9):
                assert text_of(man_call(ai, name="many", section=f"m{i}")) == \
                    "page many\n"
            assert runs("many") == (9, 1), runs("many")
            for i in range(9):
                man_call(ai, name="many", section=f"m{i}")
            assert runs("many") == (10, 1), runs("many")

            # Asks for a page under way wait for its one fetch.
            texts = []
            threads = [threading.Thread(target=lambda: texts.append(
                text_of(man_call(ai, name="slow")))) for _ in range(8)]
            for thread in threads:
                thread.start()
            for thread in threads:
                thread.join(10)
            assert texts == ["slow\n"] * 8, texts
            assert runs("slow") == (1, 1), runs("slow")

            # Arguments a model gets back to correct.
            name = "The name argument must name a manual page"
            section = "The section argument must name a section"
            for arguments, problem in (({"name": "-l"}, name), ({"name": "a/b"}, name),
                                       ({"name": ""}, name), ({"name": "x" * 129}, name),
                                       ({"name": "a\u0000b"}, name), ({"section": "1"}, name),
                                       ({"name": 1}, name),
                                       ({"name": "ls", "section": "1;x"}, section),
                                       ({"name": "ls", "section": "-1"}, section),
                                       ({"name": "ls", "section": "x" * 17}, section),
                                       ({"name": "ls", "section": 12}, section),
                                       ({"name": "ls", "section": True}, section)):
                result = man_call(ai, **arguments)
                assert result.get("isError") and text_of(result).startswith(problem), \
                    (arguments, result)
            _, _, body = ai.rpc("tools/call", {"name": "man", "arguments": [1]})
            assert text_of(body["result"]) == "Tool arguments must be an object.", body

            # What man says, and what it does wrong; none of it is kept.
            for arguments, text in (({"name": "missing", "section": "3"},
                                     "No manual entry for missing in section 3."),
                                    ({"name": "missing"}, "No manual entry for missing."),
                                    ({"name": "fail"}, "man failed, with exit status 3."),
                                    ({"name": "broken"}, "man failed, with exit status 3."),
                                    ({"name": "nowhere"},
                                     "man named no file for the page.")):
                for _ in range(2):
                    result = man_call(ai, **arguments)
                    assert result.get("isError") and text_of(result) == text, result
            assert runs("missing") == (4, 0) and runs("broken") == (2, 2)

            # A page is cut at 2 MiB, and made UTF-8 text.
            note = "\n\n[The page goes on past the 2 MiB shown.]\n"
            for _ in range(2):
                result = man_call(ai, name="big")
                assert not result.get("isError") and text_of(result) == \
                    "x" * (2 << 20) + note
            result = man_call(ai, name="endless")
            assert text_of(result) == "y\n" * (1 << 20) + note, len(text_of(result))
            assert text_of(man_call(ai, name="bytes")) == "a?b?(c€"
            assert runs("big") == (1, 1), runs("big")

            # One call waits out its deadline, and fifteen more go: sixteen
            # children at most, which go on until their deadline.
            late = []
            start = time.monotonic()

            # Its reply comes as its deadline does, after 10 s: the client
            # waits longer than that.
            def late_call():
                conn = http.client.HTTPConnection(ai.host, ai.port, timeout=15)
                status, _, body = ai.rpc("tools/call", {
                    "name": "man", "arguments": {"name": "wait0"}}, conn=conn)
                conn.close()
                assert status == 200, (status, body)
                late.append((body["result"], time.monotonic() - start))
            thread = threading.Thread(target=late_call)
            thread.start()
            wait_for(lambda: len(waiters()) == 1)
            socks = []
            for i in range(1, 16):
                sock = socket.create_connection(("127.0.0.1", ai.port), 10)
                sock.sendall(rpc_request(i, {"name": "man", "arguments": {
                    "name": f"wait{i}"}}, "tools/call"))
                socks.append(sock)
            wait_for(lambda: len(waiters()) == 16)
            result = man_call(ai, name="spare")
            assert result.get("isError") and text_of(result) == \
                "Too many pages are being rendered; try again in a few seconds."
            for sock in socks:
                sock.close()
            assert text_of(man_call(ai, name="echo")) == page
            thread.join(15)
            result, took = late[0]
            assert result.get("isError"), result
            assert text_of(result) == "man took longer than 10 s.", result
            assert 10 <= took < 12, took
            wait_for(lambda: not any(alive(p) for line in waiters() for p in line))
            assert text_of(man_call(ai, name="spare")) == "page spare\n"
            assert all(alive(p) for p in (ai.proc.pid,))

        # A turn's call is given what man says whole, whatever its offset.
        replies = [(200, turn_stream([(str(off), "man", json.dumps(
            {"name": "missing", "offset": off})) for off in (5, 100000)]))]
        up.reply = lambda request: replies.pop() if replies else \
            (200, turn_stream(text="A"))
        with Askai(binary, "-u", up.url(), "--man", "--upstream-tools", env=env) as ai:
            count = len(up.requests)
            assert ai.ask()["content"][0]["text"] == "A"
            results = up.requests[count + 1]["body"]["messages"][2:]
            assert [m["content"] for m in results] == \
                ["No manual entry for missing."] * 2, results
        up.reply = lambda request: (200, ANSWERS["openai"])

        # A name is found anew once the recheck has passed, its page given
        # meanwhile: a page installed since, which man finds first, takes
        # over.
        with Askai(binary, "-u", up.url(), "--man", "--test-man-recheck", "1",
                   env=env) as ai:
            assert text_of(man_call(ai, name="shadow")) == "first\n"
            open(f"{fake}/moved", "w").close()
            assert text_of(man_call(ai, name="shadow")) == "first\n"
            assert runs("shadow") == (1, 1), runs("shadow")
            time.sleep(1.1)
            assert text_of(man_call(ai, name="shadow")) == "first\n"
            wait_for(lambda: ai.metrics()["askai_man_pages"] == 2)
            assert runs("shadow") == (2, 1) and runs("shadowed") == (0, 1)
            assert text_of(man_call(ai, name="shadow")) == "second\n"

        # Stopping ends a man call, killing its child.
        os.unlink(f"{fake}/waiting")
        ai = Askai(binary, "-u", up.url(), "--man", env=env)
        with socket.create_connection(("127.0.0.1", ai.port), 10) as sock:
            sock.sendall(rpc_request(7, {"name": "man", "arguments": {"name": "wait"}},
                                     "tools/call"))
            wait_for(lambda: len(waiters()) == 1)
            ai.stop()
            status, body = Replies(sock).next()
            assert status == 503 and body["error"]["message"] == \
                "Server is shutting down", (status, body)
        wait_for(lambda: not any(alive(p) for p in waiters()[0]))

    print("man: offered with --man, arguments, environment, pages kept by file "
          "and rechecked, one fetch a page, cap, mending, deadline, why "
          "whole to a turn, and stopping: PASS")

    # The real man, if it has printf(3).
    if not shutil.which("man") or subprocess.run(
            ["man", "-w", "3", "printf"], capture_output=True).returncode:
        print("man: the real man's pages: SKIP (no printf(3))")
        return
    with Askai(binary, "-u", up.url(), "--man") as ai:
        text = text_of(man_call(ai, name="printf", section="3"))
        assert "SYNOPSIS" in text and "#include <stdio.h>" in text, text[:1000]
        assert not re.search(r"[\x00-\x09\x0b-\x1f\x7f]", text)
        assert max(len(line) for line in text.split("\n")) <= 80
        assert text_of(man_call(ai, name="printf", section=3)) == text
        assert ai.metrics()["askai_man_asks_total{result=\"hit\"}"] == 1
        result = man_call(ai, name="no_such_page_xyz", section="3")
        assert result.get("isError") and text_of(result) == \
            "No manual entry for no_such_page_xyz in section 3.", result
        # The longest page of many a system comes through whole.
        if not subprocess.run(["man", "-w", "1", "gcc"], capture_output=True).returncode:
            text = text_of(man_call(ai, name="gcc", section="1"))
            assert "SYNOPSIS" in text and not text.endswith("shown.]\n"), len(text)
    print("man: the real man's pages, UTF-8 text 80 columns wide, kept: PASS")


FAKE_GIT = r"""#!/bin/sh
# A stand-in for git: what the first revision names it does, else the real
# git does the command.
dir=${0%/*}
rev= seen=
for a; do
    [ -n "$seen" ] && { rev=$a; break; }
    [ "$a" = --end-of-options ] && seen=1
done
case $rev in
echo)
    for a; do printf '%s\n' "$a"; done
    echo ---
    env | sort
    echo ---
    ls /proc/self/fd | tr '\n' ' '
    echo
    readlink /proc/self/fd/0
    [ "$(readlink /proc/$$/fd/2)" = "$(readlink /proc/$$/fd/1)" ] && echo "2>&1"
    echo "session $(cut -d' ' -f6 /proc/$$/stat) of $$"
    grep -E '^Sig(Blk|Ign)' /proc/self/status ;;
stubborn|polite)
    echo $$ > "$dir/pid"
    if [ $rev = stubborn ]; then
        trap 'echo TERM >> "$dir/signals"' TERM
    else
        trap 'echo TERM >> "$dir/signals"; exit 143' TERM
    fi
    echo begun
    while :; do sleep 0.05; done ;;
*) exec /usr/bin/git "$@" ;;
esac
"""


def check_git(binary, up):
    """The git tool, with --git only: its arguments checked against the
    allowlists, revisions after --end-of-options and paths after --, run with
    a fixed environment and the settings that stop programs a repository
    could name; its output, errors included, made UTF-8 text and cut at
    2 MiB; at the deadline SIGTERM, then SIGKILL a second later; a call that
    goes, or askai stopping, kills it; and the repositories' names checked
    as askai starts."""
    def run(*args):
        return subprocess.run([binary, "-u", up.url(), *args], capture_output=True,
                              text=True, timeout=10)

    with Askai(binary, "-u", up.url()) as ai:
        tools = ai.rpc("tools/list")[2]["result"]["tools"]
        assert [t["name"] for t in tools] == ["ask_ai"], tools
    with tempfile.TemporaryDirectory() as d:
        git = {"GIT_CONFIG_GLOBAL": "/dev/null", "GIT_CONFIG_NOSYSTEM": "1"}
        repo = f"{d}/proj.git"
        os.mkdir(repo)
        def sh(*args):
            subprocess.run(["git", "-C", repo, "-c", "user.name=t", "-c",
                            "user.email=t@t", *args], check=True, env=os.environ | git,
                           capture_output=True)
        sh("init", "-q", "-b", "main")
        with open(f"{repo}/a.txt", "wb") as f:
            f.write(b"alpha\nbeta \xff\xfe gamma\n")
        with open(f"{repo}/big.txt", "wb") as f:
            f.write(b"x" * (3 << 20))
        sh("add", "a.txt", "big.txt")
        sh("commit", "-q", "-m", "first")
        subprocess.run(["git", "init", "-q", f"{d}/other"], check=True,
                       env=os.environ | git)
        os.mkdir(f"{d}/plain")
        for args, message in ((("--git", repo, "--tool-name", "git"),
                               "--tool-name: askai has another tool named git"),
                              (("--git", f"{d}/none"), "--git: not a git repository"),
                              (("--git", f"{d}/plain"), "--git: not a git repository"),
                              (("--git", "/"), "--git: / names no repository"),
                              (("--git", f"{d}/.git"), "names no repository"),
                              (("--git", repo, "--git-timeout", "0"),
                               "invalid --git-timeout")):
            proc = run(*args)
            assert proc.returncode == 2 and message in proc.stderr, (args, proc)
        os.mkdir(f"{d}/x")
        subprocess.run(["git", "init", "-q", f"{d}/x/proj"], check=True,
                       env=os.environ | git)
        proc = run("--git", repo, "--git", f"{d}/x/proj")
        assert proc.returncode == 2 and \
            "--git: two repositories named proj" in proc.stderr, proc

        fake = f"{d}/fake"
        os.mkdir(fake)
        with open(f"{fake}/git", "w") as f:
            f.write(FAKE_GIT)
        os.chmod(f"{fake}/git", 0o755)
        env = {"PATH": fake + ":" + os.environ.get("PATH", "/usr/bin:/bin"),
               "GIT_DIR": "/nowhere", "GIT_EXTERNAL_DIFF": "x", "HOME": d}

        def call(ai, **arguments):
            status, _, body = ai.rpc("tools/call", {"name": "git",
                                                    "arguments": arguments})
            assert status == 200, (status, body)
            return body["result"]

        with Askai(binary, "-u", up.url(), "--git", repo, "--git", f"{d}/other",
                   "--git-timeout", "1", env=env) as ai:
            tools = ai.rpc("tools/list")[2]["result"]["tools"]
            assert [t["name"] for t in tools] == ["ask_ai", "git"], tools
            schema = tools[1]["inputSchema"]
            assert schema["properties"]["repo"]["enum"] == ["proj", "other"] and \
                schema["required"] == ["repo", "command"], schema

            # What a call may ask for.
            for arguments, text in (
                    (["x"], "Tool arguments must be an object."),
                    ({"repo": "proj", "command": "log", "x": 1},
                     "Unknown argument: x."),
                    ({"repo": "nope", "command": "log"},
                     "The repo argument must name one of the repositories: "
                     "proj, other."),
                    ({"repo": "proj", "command": "push"},
                     "The command argument must be one of log, show, diff, grep, "
                     "blame, ls-files, ls-tree, cat-file and rev-parse."),
                    ({"repo": "proj", "command": "log", "options": ["--output=/x"]},
                     "git log takes no option --output=/x here."),
                    ({"repo": "proj", "command": "log", "options": ["-n", "3"]},
                     "git log takes no option -n here."),
                    ({"repo": "proj", "command": "grep", "pattern": "a",
                      "options": ["-f"]}, "git grep takes no option -f here."),
                    ({"repo": "proj", "command": "grep", "pattern": "a",
                      "options": ["--no-index"]},
                     "git grep takes no option --no-index here."),
                    ({"repo": "proj", "command": "blame", "options": ["--contents=x"]},
                     "git blame takes no option --contents=x here."),
                    ({"repo": "proj", "command": "log", "options": "--all"},
                     "The options, revisions and paths arguments must be arrays of "
                     "strings, at most 16, 4 and 32 of them, each of 1 to 1024 "
                     "bytes."),
                    ({"repo": "proj", "command": "log", "revisions": ["a\u0000b"]},
                     "The options, revisions and paths arguments must be arrays of "
                     "strings, at most 16, 4 and 32 of them, each of 1 to 1024 "
                     "bytes."),
                    ({"repo": "proj", "command": "show", "revisions": ["--help"]},
                     "A revision may not start with - or /, nor have a .. "
                     "component."),
                    ({"repo": "proj", "command": "diff", "revisions": ["/etc"]},
                     "A revision may not start with - or /, nor have a .. "
                     "component."),
                    ({"repo": "proj", "command": "diff", "revisions": ["x/../.."]},
                     "A revision may not start with - or /, nor have a .. "
                     "component."),
                    ({"repo": "proj", "command": "log", "paths": ["../x"]},
                     "A path must be relative to the repository's top, with no .. "
                     "component."),
                    ({"repo": "proj", "command": "log", "paths": ["/etc/passwd"]},
                     "A path must be relative to the repository's top, with no .. "
                     "component."),
                    ({"repo": "proj", "command": "ls-files", "revisions": ["HEAD"]},
                     "git ls-files takes no revisions."),
                    ({"repo": "proj", "command": "cat-file", "paths": ["a.txt"]},
                     "git cat-file takes no paths."),
                    ({"repo": "proj", "command": "grep"},
                     "git grep needs a pattern argument: a string of 1 to 1024 "
                     "bytes."),
                    ({"repo": "proj", "command": "log", "pattern": "a"},
                     "Only git grep takes a pattern.")):
                result = call(ai, **arguments) if isinstance(arguments, dict) else \
                    ai.rpc("tools/call", {"name": "git", "arguments": arguments})[2][
                        "result"]
                assert result.get("isError") and text_of(result) == text, \
                    (arguments, result)

            # Its argv, environment, descriptors, signals and session.
            out = text_of(call(ai, repo="proj", command="log",
                               options=["--oneline"], revisions=["echo"],
                               paths=["a.txt"]))
            argv, environ, rest = out.split("---\n")
            real = os.path.realpath(repo)
            assert argv.split("\n")[:-1] == [
                "--no-pager", "-c", "core.fsmonitor=false", "-c",
                "log.showSignature=false", "-c", "gpg.program=false", "-c",
                "gpg.ssh.program=false", "-c", "gpg.x509.program=false", "-c",
                "core.hooksPath=/dev/null", "-c", "protocol.allow=never", "-c",
                "diff.submodule=short", "-c", "diff.autoRefreshIndex=false", "-c",
                "gc.auto=0", "-c", "maintenance.auto=false", "-c",
                f"safe.directory={real}", "-C", real, "log", "--no-ext-diff",
                "--no-textconv", "--oneline", "--end-of-options", "echo", "--",
                "a.txt"], argv
            environ = [v for v in environ.split("\n")[:-1]
                       if not v.startswith(("PWD=", "OLDPWD=", "SHLVL=", "_="))]
            top = os.path.realpath(d)
            assert environ == ["GIT_ALLOW_PROTOCOL=none",
                               f"GIT_CEILING_DIRECTORIES={top}:{top}",
                               "GIT_CONFIG_GLOBAL=/dev/null", "GIT_CONFIG_NOSYSTEM=1",
                               "GIT_NO_LAZY_FETCH=1", "GIT_OPTIONAL_LOCKS=0",
                               "GIT_PAGER=cat", "GIT_TERMINAL_PROMPT=0",
                               "HOME=/nonexistent", "LC_ALL=C.UTF-8",
                               "PATH=" + env["PATH"]], environ
            lines = rest.split("\n")
            assert lines[0] == "0 1 2 3 " and lines[1] == "/dev/null" and \
                lines[2] == "2>&1", lines
            sid, pid = re.fullmatch(r"session (\d+) of (\d+)", lines[3]).groups()
            assert sid == pid, lines
            assert lines[4:6] == ["SigBlk:\t0000000000000000",
                                  "SigIgn:\t0000000000000000"], lines

            # The real git: output, errors, mending, the cut, and grep's none.
            assert text_of(call(ai, repo="proj", command="log",
                                options=["--format=%s"])) == "first\n"
            assert text_of(call(ai, repo="proj", command="grep", pattern="beta",
                                options=["-n"])) == "a.txt:2:beta ?? gamma\n"
            result = call(ai, repo="proj", command="grep", pattern="nothing here")
            assert not result.get("isError") and \
                text_of(result) == "[git exited with status 1.]", result
            result = call(ai, repo="proj", command="show", revisions=["nosuch"])
            assert result.get("isError") and text_of(result).startswith(
                "fatal: bad revision 'nosuch'\n\n[git exited with status 128.]"), result
            result = call(ai, repo="proj", command="cat-file", options=["-p"],
                          revisions=["HEAD:big.txt"])
            assert not result.get("isError") and text_of(result) == "x" * (2 << 20) + \
                "\n\n[The output goes on past the 2 MiB shown.]", len(text_of(result))
            result = call(ai, repo="other", command="log")
            assert result.get("isError") and "does not have any commits" in \
                text_of(result), result
            assert text_of(call(ai, repo="proj", command="ls-files")) == \
                "a.txt\nbig.txt\n"
            assert text_of(call(ai, repo="proj", command="ls-tree",
                                options=["--name-only"], revisions=["HEAD"],
                                paths=["a.txt"])) == "a.txt\n"
            head = subprocess.run(["git", "-C", repo, "rev-parse", "HEAD"], text=True,
                                  capture_output=True, env=os.environ | git).stdout
            assert text_of(call(ai, repo="proj", command="rev-parse",
                                revisions=["HEAD"])) == head
            result = call(ai, repo="proj", command="rev-parse", revisions=["a.txt"])
            assert text_of(result) == "[No output.]", result

            # The deadline: SIGTERM, then SIGKILL a second later, to one that
            # stays; one that goes on SIGTERM is gone at once.
            for rev, least, most in (("stubborn", 1.9, 3.5), ("polite", 0.9, 1.9)):
                start = time.monotonic()
                result = call(ai, repo="proj", command="log", revisions=[rev])
                took = time.monotonic() - start
                text = text_of(result)
                assert result.get("isError") and text.startswith("begun\n") and \
                    text.endswith("\n\n[git took longer than 1 s, and was stopped.]"), \
                    result
                assert least <= took < most, (rev, took)
                with open(f"{fake}/signals") as f:
                    assert f.read() == "TERM\n"
                os.unlink(f"{fake}/signals")
                with open(f"{fake}/pid") as f:
                    pid = int(f.read())
                wait_for(lambda: not alive(pid))

            # A call that goes kills its git.
            os.unlink(f"{fake}/pid")
            with socket.create_connection(("127.0.0.1", ai.port), 10) as sock:
                sock.sendall(rpc_request(7, {"name": "git", "arguments": {
                    "repo": "proj", "command": "log", "revisions": ["stubborn"]}},
                    "tools/call"))
                wait_for(lambda: os.path.exists(f"{fake}/pid"))
                with open(f"{fake}/pid") as f:
                    pid = int(f.read())
            wait_for(lambda: not alive(pid))
            assert not os.path.exists(f"{fake}/signals")

        # Stopping askai ends a call, killing its git; a configuration file
        # lists the repositories too.
        with open(f"{d}/askai.json", "w") as f:
            json.dump({"git": [repo, f"{d}/other"]}, f)
        os.chmod(f"{d}/askai.json", 0o600)
        os.unlink(f"{fake}/pid")
        ai = Askai(binary, "-u", up.url(), "-c", f"{d}/askai.json", env=env)
        with socket.create_connection(("127.0.0.1", ai.port), 10) as sock:
            sock.sendall(rpc_request(8, {"name": "git", "arguments": {
                "repo": "other", "command": "log", "revisions": ["stubborn"]}},
                "tools/call"))
            wait_for(lambda: os.path.exists(f"{fake}/pid"))
            with open(f"{fake}/pid") as f:
                pid = int(f.read())
            ai.stop()
            status, body = Replies(sock).next()
            assert status in (200, 503), (status, body)
        wait_for(lambda: not alive(pid))
    print("git: offered with --git, arguments allowed and refused, argv, "
          "environment, output, errors, mending, cut, deadline, cancel, stop: "
          "PASS")


def turn_stream(calls=(), text="", reasoning="", used=None, interleave=False,
                index=True, reason=None, extras=None):
    """A streamed chat completion of @p text that calls the tools @p calls,
    (id, name, arguments' JSON text) each, the arguments in pieces of 7
    bytes, after @p reasoning; with @p used, (prompt, answer) tokens, its
    usage; with @p interleave, the calls' pieces taken in turn; without
    @p index, the calls' pieces giving none; with @p reason, that stop
    reason; with @p extras, each call's extra_content or None, as Gemini
    sends them: each call whole in one piece, with no index."""
    ev = [(None, {"choices": [{"index": 0, "delta": {"role": "assistant",
                                                     "content": ""}}]})]
    delta = lambda d: (None, {"choices": [{"index": 0, "delta": d}]})
    # As servers stream: many small pieces.
    ev += [delta({"reasoning_content": reasoning[k:k + (16 << 10)]})
           for k in range(0, len(reasoning), 16 << 10)]
    ev += [delta({"content": text[k:k + (16 << 10)]})
           for k in range(0, len(text), 16 << 10)]
    pieces = []
    for (cid, name, args), extra in zip(calls, extras or ()):
        whole = {"id": cid, "type": "function",
                 "function": {"name": name, "arguments": args}}
        pieces.append([delta({"tool_calls": [whole | (
            {"extra_content": extra} if extra else {})]})])
    for i, (cid, name, args) in enumerate(calls if not extras else ()):
        at = {"index": i} if index else {}
        call = [delta({"tool_calls": [at | {"id": cid, "type": "function",
                                            "function": {"name": name,
                                                         "arguments": ""}}]})]
        call += [delta({"tool_calls": [at | {"function": {
            "arguments": args[k:k + 7]}}]}) for k in range(0, len(args), 7)]
        pieces.append(call)
    if interleave:
        ev += [e for group in zip(*pieces) for e in group]
    else:
        ev += [e for call in pieces for e in call]
    ev.append((None, {"choices": [{"index": 0, "delta": {}, "finish_reason":
                                   reason or ("tool_calls" if calls else "stop")}]}))
    if used:
        ev.append((None, {"choices": [], "usage": {"prompt_tokens": used[0],
                                                   "completion_tokens": used[1]}}))
    ev.append((None, "[DONE]"))
    return Stream(wire=sse_wire(ev))


def messages_turn(blocks, stop="tool_use", stray=False):
    """A streamed answer of Anthropic's Messages of the content @p blocks,
    each ("thinking", text, signature), ("redacted", data), ("text", text),
    ("tool", id, name, arguments' JSON text) or ("whole", id, name,
    arguments' JSON text), whose arguments come with its start, the texts
    and arguments in pieces of 5 bytes, that stops for @p stop; with
    @p stray, a text delta with no index after each block's start."""
    ev = [("message_start", {"type": "message_start", "message": {
        "id": "msg", "type": "message", "role": "assistant", "content": [],
        "usage": {"input_tokens": 90, "cache_read_input_tokens": 10,
                  "output_tokens": 1}}})]
    starts = {"thinking": lambda b: {"type": "thinking", "thinking": "",
                                     "signature": ""},
              "redacted": lambda b: {"type": "redacted_thinking", "data": b[1]},
              "text": lambda b: {"type": "text", "text": ""},
              "tool": lambda b: {"type": "tool_use", "id": b[1], "name": b[2],
                                 "input": {}},
              "whole": lambda b: {"type": "tool_use", "id": b[1], "name": b[2],
                                  "input": json.loads(b[3])}}
    pieces = {"thinking": lambda b: [{"type": "thinking_delta", "thinking": t}
                                     for t in split(b[1], 5)] +
                                    [{"type": "signature_delta", "signature": b[2]}],
              "redacted": lambda b: [],
              "text": lambda b: [{"type": "text_delta", "text": t}
                                 for t in split(b[1], 5)],
              "tool": lambda b: [{"type": "input_json_delta", "partial_json": t}
                                 for t in split(b[3], 5)],
              "whole": lambda b: []}
    for i, b in enumerate(blocks):
        ev.append(("content_block_start", {"type": "content_block_start", "index": i,
                                           "content_block": starts[b[0]](b)}))
        if stray:
            ev.append(("content_block_delta", {"type": "content_block_delta",
                                               "delta": {"type": "text_delta",
                                                         "text": "!"}}))
        ev += [("content_block_delta", {"type": "content_block_delta", "index": i,
                                        "delta": d}) for d in pieces[b[0]](b)]
        ev.append(("content_block_stop", {"type": "content_block_stop", "index": i}))
    ev.append(("message_delta", {"type": "message_delta",
                                 "delta": {"stop_reason": stop},
                                 "usage": {"output_tokens": 20}}))
    ev.append(("message_stop", {"type": "message_stop"}))
    return Stream(wire=sse_wire(ev))


def split(text, n):
    """@p text in pieces of @p n characters; one empty piece if it is empty."""
    return [text[k:k + n] for k in range(0, len(text), n)] or [""]


def responses_turn(items, text=""):
    """A streamed answer of the Responses API of @p text, then the output
    @p items, each done whole."""
    ev = [(None, {"type": "response.created", "response": {"status": "in_progress"}})]
    ev += [(None, {"type": "response.output_text.delta", "output_index": 0,
                   "delta": t}) for t in split(text, 5) if t]
    ev += [(None, {"type": "response.output_item.done", "output_index": i,
                   "item": item}) for i, item in enumerate(items)]
    ev.append((None, {"type": "response.completed", "response": {
        "status": "completed", "usage": {"input_tokens": 100, "output_tokens": 20}}}))
    return Stream(wire=sse_wire(ev))


def generate_turn(chunks, reason="STOP"):
    """A streamed answer of Gemini's of @p chunks, each a list of parts, the
    last with @p reason."""
    ev = [(None, {"candidates": [{"content": {"role": "model", "parts": parts}} |
                                 ({"finishReason": reason} if i + 1 == len(chunks)
                                  else {})],
                  "usageMetadata": {"totalTokenCount": 120}})
          for i, parts in enumerate(chunks)]
    return Stream(wire=sse_wire(ev))


def check_turn_apis(binary, up):
    """With --upstream-tools, the turns of the Responses API, Anthropic's
    Messages and Gemini's generateContent: the tools are offered as each
    has them, a turn goes back as it came, its items in their order, its
    thinking and its signatures whole, and its calls' results as each
    takes them; an answer that the turns doze after goes back as it came,
    then the message that wakes them, as the user's."""
    with tempfile.TemporaryDirectory() as d:
        git = {"GIT_CONFIG_GLOBAL": "/dev/null", "GIT_CONFIG_NOSYSTEM": "1"}
        repo = f"{d}/proj"
        os.mkdir(repo)
        for args in (("init", "-q", "-b", "main"), ("commit", "-q", "--allow-empty",
                                                     "-m", "first")):
            subprocess.run(["git", "-C", repo, "-c", "user.name=t", "-c",
                            "user.email=t@t", *args], check=True,
                           env=os.environ | git, capture_output=True)
        log = {"repo": "proj", "command": "log", "options": ["--oneline"]}
        logged = r"[0-9a-f]{7} first\n"
        script = []

        def reply(request):
            return script.pop(0)(request)
        up.reply = reply

        def run(api, path):
            return Askai(binary, "--api", api, "-u", up.url(path), "--upstream-tools",
                         "--git", repo)
        woke = "[askai: timer 1 fired.]\n"

        def doze(ai, answers, key):
            """An ask call whose turns, the @p answers, set a timer of 1 s,
            answer "Waiting." and doze until it fires, then answer "A": the
            conversation, @p key in its body, that woke them."""
            script[:] = [lambda r, a=a: (200, a) for a in answers]
            count = len(up.requests)
            result = ai.ask("Q")
            assert result["content"][0]["text"] == f"Waiting.\n\n{woke}\nA", result
            return up.requests[count + 2]["body"][key]

        # The Responses API: the turn is its output items, in the input's
        # list, its calls' outputs after them.
        items = [{"type": "reasoning", "id": "rs_1", "summary": [],
                  "encrypted_content": "enc/+="},
                 {"type": "message", "id": "msg_1", "role": "assistant",
                  "status": "completed",
                  "content": [{"type": "output_text", "text": "Looking.",
                               "annotations": []}]},
                 {"type": "function_call", "id": "fc_1", "call_id": "c1",
                  "name": "git", "arguments": json.dumps(log), "status": "completed"},
                 {"type": "function_call", "id": "fc_2", "call_id": "c2",
                  "name": "git", "arguments": "{\"repo\": \"proj\", \"comm"}]
        with run("responses", "/v1/responses") as ai:
            script[:] = [lambda r: (200, responses_turn(items, "Looking.")),
                         lambda r: (200, responses_turn([], "A"))]
            count = len(up.requests)
            result = ai.ask("Q")
            assert result["content"][0]["text"] == "A", result
            first, second = up.requests[count:]
            tools = first["body"]["tools"]
            assert [(t["type"], t["name"]) for t in tools] == \
                [("function", t) for t in TURN_TOOLS], tools
            assert tools[0]["parameters"]["properties"]["repo"], tools
            given = second["body"]["input"]
            assert list(second["body"])[-1] == "input", second["body"]
            assert given[0] == {"role": "user", "content": "Q"}, given
            assert given[1:5] == items, given
            assert [(g["type"], g["call_id"]) for g in given[5:]] == \
                [("function_call_output", "c1"), ("function_call_output", "c2")], given
            assert re.fullmatch(logged, given[5]["output"]), given
            assert given[6]["output"] == "Your arguments were not a JSON object, " \
                "so the call was not run.", given
            timer = {"type": "function_call", "id": "fc_3", "call_id": "t1",
                     "name": "timer", "arguments": "{\"seconds\":1}",
                     "status": "completed"}
            given = doze(ai, [responses_turn([timer]),
                              responses_turn([items[1]], "Waiting."),
                              responses_turn([], "A")], "input")
            assert given[3:] == [items[1], {"role": "user", "content": woke}], given

        # Anthropic's Messages: thinking blocks, signed, and a redacted one go
        # back in their places among the text and the calls, but a blank
        # text block, and the text of a delta with no index; arguments may
        # come with the call's start; the results go in one message.
        blocks = [("thinking", "Plan it.", "sig/1"), ("text", "Looking."), ("text", ""),
                  ("text", " \n\n"), ("thinking", "", "sig2"),
                  ("tool", "c1", "git", json.dumps(log)), ("redacted", "opaque=="),
                  ("tool", "c2", "git", "{\"repo\":"),
                  ("whole", "c3", "git", json.dumps(log))]
        with run("messages", "/v1/messages") as ai:
            script[:] = [lambda r: (200, messages_turn(blocks, stray=True)),
                         lambda r: (200, messages_turn([("text", "A")], "end_turn"))]
            count = len(up.requests)
            result = ai.ask("Q")
            assert result["content"][0]["text"] == "A", result
            first, second = up.requests[count:]
            tools = first["body"]["tools"]
            assert [t["name"] for t in tools] == TURN_TOOLS and \
                tools[0]["input_schema"]["properties"]["repo"], tools
            messages = second["body"]["messages"]
            assert list(second["body"])[-1] == "messages", second["body"]
            assert messages[0] == {"role": "user", "content": "Q"}, messages
            assert messages[1] == {"role": "assistant", "content": [
                {"type": "thinking", "thinking": "Plan it.", "signature": "sig/1"},
                {"type": "text", "text": "Looking."},
                {"type": "thinking", "thinking": "", "signature": "sig2"},
                {"type": "tool_use", "id": "c1", "name": "git", "input": log},
                {"type": "redacted_thinking", "data": "opaque=="},
                {"type": "tool_use", "id": "c2", "name": "git", "input": {}},
                {"type": "tool_use", "id": "c3", "name": "git", "input": log}]}, messages
            assert b'"signature":"sig/1"' in second["raw"], second["raw"]
            results = messages[2]
            assert results["role"] == "user" and len(messages) == 3, messages
            assert [(c["type"], c["tool_use_id"]) for c in results["content"]] == \
                [("tool_result", "c1"), ("tool_result", "c2"), ("tool_result", "c3")], \
                results
            assert re.fullmatch(logged, results["content"][0]["content"]), results
            assert re.fullmatch(logged, results["content"][2]["content"]), results
            messages = doze(ai, [messages_turn([("tool", "t1", "timer", '{"seconds":1}')]),
                                 messages_turn([("text", "Waiting.")], "end_turn"),
                                 messages_turn([("text", "A")], "end_turn")], "messages")
            assert messages[3:] == [
                {"role": "assistant", "content": [{"type": "text", "text": "Waiting."}]},
                {"role": "user", "content": woke}], messages

        # Gemini's: text parts go on in one, but a part with a thought
        # signature, thinking too, goes back as it came, as do the calls;
        # their responses name them, and give the id of one that had one.
        call = {"name": "git", "args": log}
        turns = [
            [[{"text": "Plan.", "thought": True}],
             [{"text": "Deep.", "thought": True, "thoughtSignature": "thsig"}],
             [{"text": "Lo"}], [{"text": "ok"}],
             [{"text": "ing.", "thoughtSignature": "tsig"}],
             [{"functionCall": call, "thoughtSignature": "fsig/+"},
              {"functionCall": call}]],
            [[{"text": "More"}], [{"text": "."},
                                  {"functionCall": call | {"id": "g3"}}]],
        ]
        with run("generate", "/v1beta/models/m:generateContent") as ai:
            script[:] = [lambda r: (200, generate_turn(turns[0])),
                         lambda r: (200, generate_turn(turns[1])),
                         lambda r: (200, generate_turn([[{"text": "A"}]]))]
            count = len(up.requests)
            result = ai.ask("Q")
            assert result["content"][0]["text"] == "A", result
            first, second, third = up.requests[count:]
            tools = first["body"]["tools"]
            declared = tools[0]["functionDeclarations"]
            assert len(tools) == 1 and [t["name"] for t in declared] == \
                TURN_TOOLS, tools
            assert declared[0]["parametersJsonSchema"]["properties"]["repo"], tools
            contents = second["body"]["contents"]
            assert list(second["body"])[-1] == "contents", second["body"]
            assert contents[0] == {"role": "user", "parts": [{"text": "Q"}]}, contents
            assert contents[1] == {"role": "model", "parts": [
                {"text": "Deep.", "thought": True, "thoughtSignature": "thsig"},
                {"text": "Look"}, {"text": "ing.", "thoughtSignature": "tsig"},
                {"functionCall": call, "thoughtSignature": "fsig/+"},
                {"functionCall": call}]}, contents
            parts = contents[2]["parts"]
            assert contents[2]["role"] == "user" and len(contents) == 3, contents
            assert [p["functionResponse"]["name"] for p in parts] == ["git", "git"] and \
                all("id" not in p["functionResponse"] for p in parts), parts
            assert re.fullmatch(logged, parts[0]["functionResponse"]["response"]["output"]), \
                parts
            contents = third["body"]["contents"]
            assert contents[3] == {"role": "model", "parts": [
                {"text": "More."}, {"functionCall": call | {"id": "g3"}}]}, contents
            assert contents[4]["parts"][0]["functionResponse"]["id"] == "g3", contents
            contents = doze(ai, [generate_turn([[{"functionCall": {
                                     "name": "timer", "args": {"seconds": 1}}}]]),
                                 generate_turn([[{"text": "Waiting."}]]),
                                 generate_turn([[{"text": "A"}]])], "contents")
            assert contents[3:] == [{"role": "model", "parts": [{"text": "Waiting."}]},
                                    {"role": "user", "parts": [{"text": woke}]}], contents
    up.reply = lambda request: (200, ANSWERS["openai"])
    print("turns of the Responses API, Anthropic's Messages and Gemini's: tools, "
          "items in order, thinking and signatures given back, results, waking: PASS")


def check_turns(binary, up):
    """With --upstream-tools, an ask call lends the upstream the tools its
    client's role may call but itself, and notes and compact, in chat
    completions: it runs the tools each turn calls, a tool's output cut to a
    window that offset moves, writes the turn and the results into the
    conversation, and asks on until an answer calls none, --max-turns turns
    at most; the client gets the answer and the calls' trace.  Notes are
    commits on the thread's branch, read back, and given again with the
    prompt to a call that goes on with the thread; compact, a context that
    the upstream says is full, or one askai reckons is, has the
    conversation start again from the prompt, the summary and the notes."""
    def run(*args):
        return subprocess.run([binary, "-u", up.url(), *args], capture_output=True,
                              text=True, timeout=10)

    for args, message in ((("--notes", "/nonexistent/x"), "--notes needs "
                           "--upstream-tools"),
                          (("--upstream-tools", "--api", "generate", "-x",
                            '{"contents": null}'), "--extra may not change contents "
                           "with --upstream-tools"),
                          (("--upstream-tools", "--max-turns", "0"),
                           "invalid --max-turns: 0"),
                          (("--upstream-tools", "--context", "-1"),
                           "invalid --context: -1"),
                          (("--upstream-tools", "-x", '{"messages": null}'),
                           "--extra may not change messages with --upstream-tools")):
        proc = run(*args)
        assert proc.returncode == 2 and message in proc.stderr, (args, proc)

    with tempfile.TemporaryDirectory() as d:
        git = {"GIT_CONFIG_GLOBAL": "/dev/null", "GIT_CONFIG_NOSYSTEM": "1"}
        repo = f"{d}/proj"
        os.mkdir(repo)

        def sh(*args, cwd=repo):
            return subprocess.run(["git", "-C", cwd, "-c", "user.name=t", "-c",
                                   "user.email=t@t", *args], check=True,
                                  env=os.environ | git, capture_output=True,
                                  text=True).stdout
        sh("init", "-q", "-b", "main")
        with open(f"{repo}/big.txt", "w") as f:
            f.write("".join(f"line {i:05}\n" for i in range(4000)))
        sh("add", "big.txt")
        sh("commit", "-q", "-m", "first")
        notes = f"{d}/notes"
        with open(f"{d}/file", "w"):
            pass
        os.mkdir(f"{d}/work")
        sh("init", "-q", cwd=f"{d}/work")
        for path, message in ((f"{d}/file", "not a directory"),
                              (f"{d}/work", "not a git repository"),
                              (f"{d}/work/.git", "not a bare git repository")):
            proc = run("--upstream-tools", "--notes", path)
            assert proc.returncode and message in proc.stderr, (path, proc)

        def turns(request):
            """The messages after the prompt's."""
            return request["body"]["messages"][1:]

        def prompt(request):
            return request["body"]["messages"][0]["content"]

        script = []

        def reply(request):
            return script.pop(0)(request) if script else (200, turn_stream(text="A"))
        up.reply = reply
        args = ("-u", up.url(), "--upstream-tools", "--git", repo, "--notes", notes)
        with Askai(binary, *args) as ai:
            tools = {t["name"]: t for t in ai.rpc("tools/list")[2]["result"]["tools"]}
            assert "thread" in tools["ask_ai"]["inputSchema"]["properties"], tools
            assert "offset" in tools["git"]["inputSchema"]["properties"], tools
            assert "notes" not in tools and "compact" not in tools, tools

            # Two calls, git's and a note, then a read of the notes, then the
            # answer.  The turns carry the text, the thinking and the calls.
            log = json.dumps({"repo": "proj", "command": "log",
                              "options": ["--oneline"]})
            script[:] = [
                lambda r: (200, turn_stream([("c1", "git", log), ("c2", "notes", json.dumps(
                    {"action": "write", "text": "found \"first\""}))], text="Looking.",
                    reasoning="hmm", used=(100, 20))),
                lambda r: (200, turn_stream([("", "notes", '{"action":"read"}')])),
            ]
            count = len(up.requests)
            result = ai.ask("What is there?")
            first, second, third = up.requests[count:]
            body = first["body"]
            assert [t["function"]["name"] for t in body["tools"]] == \
                ["git", "notes", "compact", "background", "jobs", "wait", "timer"], \
                body["tools"]
            assert body["stream_options"] == {"include_usage": True}, body
            assert list(body)[-1] == "messages" and turns(first) == [], body
            head, *results = turns(second)
            assert head["role"] == "assistant" and head["content"] == "Looking." and \
                head["reasoning_content"] == "hmm", head
            assert [(c["id"], c["function"]["name"]) for c in head["tool_calls"]] == \
                [("c1", "git"), ("c2", "notes")], head
            assert json.loads(head["tool_calls"][0]["function"]["arguments"]) == \
                json.loads(log), head
            assert [(m["role"], m["tool_call_id"]) for m in results] == \
                [("tool", "c1"), ("tool", "c2")], results
            assert re.fullmatch(r"[0-9a-f]{7} first\n", results[0]["content"]), results
            assert results[1]["content"] == "Noted.", results
            read = turns(third)[3:]
            assert read[0]["tool_calls"][0]["id"] == "call_2_0", read
            assert "reasoning_content" not in read[0], read
            assert read[1] == {"role": "tool", "tool_call_id": "call_2_0",
                               "content": "1. found \"first\"\n"}, read
            text, trace = [c["text"] for c in result["content"]]
            assert text == "A", result
            thread = re.search(r"in the thread ([0-9a-f]{32}):", trace).group(1)
            assert trace.startswith("The other AI made 3 tool calls in 3 turns:\n"
                                    f"- git {log}: 14 bytes\n"
                                    "- notes write: 6 bytes\n- notes read: 17 bytes\n"
                                    ), trace
            assert "found" not in trace, trace
            branch = f"refs/heads/anonymous/{thread}"
            assert sh("--git-dir", notes, "log", "--format=%B", branch,
                      cwd=d).strip() == "found \"first\"", branch

            # Going on with the thread: its notes go with the prompt.
            count = len(up.requests)
            result = ai.ask("And now?", thread=thread)
            assert prompt(up.requests[count]) == (
                "And now?\n\n[askai: the notes you kept in earlier calls on this "
                "thread, oldest first:]\n\n1. found \"first\"\n"), up.requests[count]
            assert f"in the thread {thread}:" in result["content"][1]["text"], result
            for value, message in (("0" * 32, "Unknown thread: " + "0" * 32 + "."),
                                   ("xyz", "must be a thread's id")):
                result = ai.ask("Q", thread=value)
                assert result.get("isError") and message in text_of(result), result

            # A window of git's output, the rest by offset; a note too long,
            # a tool not lent, arguments that are no object.
            show = {"repo": "proj", "command": "show", "revisions": ["HEAD:big.txt"]}
            script[:] = [
                lambda r: (200, turn_stream([
                    ("a", "git", json.dumps(show)),
                    ("b", "git", json.dumps(show | {"offset": 65530})),
                    ("c", "notes", json.dumps({"action": "write", "text": "x" * 8193})),
                    ("d", "man", '{"name":"ls"}'), ("e", "git", "[1]"),
                    ("f", "git", '{"repo":')])),
            ]
            count = len(up.requests)
            assert ai.ask("Q")["content"][0]["text"] == "A"
            calls, *results = turns(up.requests[count + 1])
            outputs = [m["content"] for m in results]
            with open(f"{repo}/big.txt") as f:
                assert outputs[0] == f.read(), outputs[0][-200:]
            assert outputs[1] == "[The output has 44000 bytes, fewer than the offset.]", \
                outputs[1]
            assert outputs[2] == "A note must be a string of 1 to 8192 bytes, and no " \
                "NUL.", outputs[2]
            assert outputs[3] == "No tool of that name is yours to call.", outputs[3]
            assert outputs[4] == outputs[5] == "Your arguments were not a JSON " \
                "object, so the call was not run.", outputs[4:]
            assert [c["function"]["arguments"] for c in calls["tool_calls"][4:]] == \
                ["{}", "{}"], calls
            _, _, body = ai.rpc("tools/call", {"name": "git", "arguments": {
                "repo": "proj", "command": "log", "offset": 999999}})
            assert body["result"].get("isError") and re.fullmatch(
                r"The output has \d+ bytes, fewer than the offset\.",
                text_of(body["result"])), body

            # Calls with no index, as some servers send them; arguments the
            # output token limit cut; a search of more than a line.
            script[:] = [
                lambda r: (200, turn_stream([("x", "git", log), ("y", "git", log)],
                                            index=False)),
                lambda r: (200, turn_stream([("z", "git", log[:-9])], reason="length")),
                lambda r: (200, turn_stream([("n", "notes", json.dumps(
                    {"action": "read", "search": "a\nb"}))])),
            ]
            count = len(up.requests)
            assert ai.ask("Q")["content"][0]["text"] == "A"
            outputs = [m["content"] for m in turns(up.requests[count + 1])[1:]]
            assert len(outputs) == 2 and outputs[0] == outputs[1], outputs
            assert turns(up.requests[count + 2])[-1]["content"] == "Your answer " \
                "reached the output token limit, which cut these arguments short, " \
                "so the call was not run.", up.requests[count + 2]
            assert turns(up.requests[count + 3])[-1]["content"] == "The search " \
                "argument, which only read takes, must be a line of 1 to 1024 " \
                "bytes.", up.requests[count + 3]

            values = ai.metrics()
            assert total(values, "askai_turns_total") == 10, values
            assert [total(values, "askai_turn_tool_calls_total", tool=t) for t in
                    ("git", "notes", "man", "compact", "unknown")] == [8, 4, 0, 0, 1], \
                values

        # A small context: windows of an eighth of it, a warning past three
        # quarters, and once full, the conversation starts again.
        with Askai(binary, *args, "--context", "8000") as ai:
            show = {"repo": "proj", "command": "show", "revisions": ["HEAD:big.txt"]}
            script[:] = [
                lambda r: (200, turn_stream([("a", "git", json.dumps(show))],
                                            used=(4000, 10))),
                lambda r: (200, turn_stream([("b", "git", json.dumps(show | {
                    "offset": 3993}))], used=(6000, 10))),
                lambda r: (200, turn_stream([("c", "git", json.dumps(show))],
                                            used=(7900, 10))),
            ]
            count = len(up.requests)
            ai.ask("Q")
            out = turns(up.requests[count + 1])[1]["content"]
            # Lines of 11 bytes: 363 of them in an eighth of 8000 tokens.
            assert out.startswith("line 00000\n") and out.endswith(
                "line 00362\n\n\n[Bytes 0 to 3993 of 44000: for the rest, call "
                "again with \"offset\": 3993.]"), out[-120:]
            out = turns(up.requests[count + 2])[3]["content"]
            assert out.startswith("line 00363\n") and \
                "line 00725\n\n\n[Bytes 3993 to 7986 of 44000: for the rest" in out and \
                re.search(r"\[askai: your context is \d+% full, \d+ of 8000 "
                          r"tokens\. Note what you need to keep, then call "
                          r"compact\.\]$", out), out[-300:]
            # Full by its reckoning: from the prompt again.
            again = up.requests[count + 3]
            assert turns(again) == [] and prompt(again) == (
                "Q\n\n[askai: your context filled up, so the conversation since the "
                "task is gone, your turns and the tools' results with it.]\n\n"
                "[askai: go on with the task.]"), again

        # Hooks in the notes repository do not run.
        os.makedirs(f"{notes}/hooks", exist_ok=True)
        with open(f"{notes}/hooks/reference-transaction", "w") as f:
            f.write(f"#!/bin/sh\ntouch {d}/hooked\n")
        os.chmod(f"{notes}/hooks/reference-transaction", 0o755)
        with Askai(binary, *args, "--max-turns", "3") as ai:
            # compact: from the prompt, the summary and the notes; calls after
            # it do not run.
            script[:] = [
                lambda r: (200, turn_stream([("a", "notes", json.dumps(
                    {"action": "write", "text": "n1"}))])),
                lambda r: (200, turn_stream([("b", "compact", json.dumps(
                    {"summary": "S"})), ("c", "git", log)])),
            ]
            count = len(up.requests)
            result = ai.ask("Q")
            again = up.requests[count + 2]
            assert turns(again) == [] and prompt(again) == (
                "Q\n\n[askai: you compacted the context: the conversation since the "
                "task is gone, your turns and the tools' results with it. Your "
                "summary of it:]\n\nS\n\n[askai: your notes, oldest first:]\n\n"
                "1. n1\n\n[askai: go on with the task.]"), prompt(again)
            assert "- compact: 1 bytes\n" in result["content"][1]["text"], result
            assert "git" not in result["content"][1]["text"], result
            assert not os.path.exists(f"{d}/hooked")

            # The upstream says the context is full: once, from the prompt
            # again; twice, the call fails.
            full = {"error": {"code": 400, "type": "exceed_context_size_error",
                              "message": "request (9000 tokens) exceeds the "
                              "available context size (8192 tokens), try "
                              "increasing it"}}
            script[:] = [lambda r: (200, turn_stream([("a", "git", log)])),
                         lambda r: (400, full)]
            count = len(up.requests)
            assert ai.ask("Q")["content"][0]["text"] == "A"
            assert turns(up.requests[count + 2]) == [], up.requests[count + 2]
            script[:] = [lambda r: (200, turn_stream([("a", "git", log)])),
                         lambda r: (400, full), lambda r: (400, full)]
            result = ai.ask("Q")
            assert result.get("isError") and "exceeds the available context" in \
                result["content"][0]["text"], result

            # The turns run out: the last is told, and stopped.
            script[:] = [lambda r: (200, turn_stream([(str(i), "git", log)]))
                         for i in range(3)]
            count = len(up.requests)
            result = ai.ask("Q")
            assert len(up.requests) == count + 3, up.requests[count:]
            assert turns(up.requests[count + 2])[-1]["content"].endswith(
                "[askai: your next turn is your last: answer then, without calling "
                "tools.]"), up.requests[count + 2]
            assert result.get("isError") and result["content"][0]["text"] == "" and \
                "It was stopped after 3 turns, still calling tools." in \
                result["content"][1]["text"], result

            # Turns that doze are told, as they wake, if their next is their
            # last, whose answer ends the call, though a timer is still set.
            script[:] = [
                lambda r: (200, turn_stream([(c, "timer", json.dumps({"seconds": s}))
                                             for c, s in (("t", 1), ("u", 60))])),
                lambda r: (200, turn_stream(text="Waiting.")),
            ]
            count = len(up.requests)
            start = time.monotonic()
            result = ai.ask("Q")
            assert time.monotonic() - start < 3, time.monotonic() - start
            assert turns(up.requests[count + 2])[-1]["content"] == (
                "[askai: timer 1 fired.]\n[askai: your next turn is your last: answer "
                "then, without calling tools.]\n"), up.requests[count + 2]
            assert result["content"][0]["text"] == \
                "Waiting.\n\n[askai: timer 1 fired.]\n\nA", result

            # Calls out of order are the upstream's error.
            script[:] = [lambda r: (200, turn_stream([("a", "git", log), ("b", "git", log)],
                                                     interleave=True))]
            result = ai.ask("Q")
            assert result.get("isError") and "tool calls out of order" in \
                result["content"][0]["text"], result

            # Gemini's thought signatures, in extra_content: whole calls with
            # no index, of which only the first of parallel ones is signed,
            # go back as they came; one that comes after its arguments is out
            # of order.
            signed = {"google": {"thought_signature": "c2ln/+=="}}
            script[:] = [lambda r: (200, turn_stream([("g1", "git", log), ("g2", "git", log)],
                                                     extras=(signed, None)))]
            count = len(up.requests)
            result = ai.ask("Q")
            assert result["content"][0]["text"] == "A", result
            request = up.requests[count + 1]
            head = turns(request)[0]
            assert head["tool_calls"][0]["extra_content"] == signed, head
            assert "extra_content" not in head["tool_calls"][1], head
            assert b'"extra_content":{"google":{"thought_signature":"c2ln/+=="}}}' in \
                request["raw"], request["raw"]
            late = turn_stream([("g", "git", log)]).wire.replace(
                b'"delta": {}, "finish_reason"', b'"delta": {"tool_calls": [{"index": 0, '
                b'"extra_content": {"google": {}}}]}, "finish_reason"', 1)
            assert b'"extra_content"' in late
            script[:] = [lambda r: (200, Stream(wire=late))]
            result = ai.ask("Q")
            assert result.get("isError") and "tool calls out of order" in \
                result["content"][0]["text"], result

            values = ai.metrics()
            assert total(values, "askai_compactions_total", by="upstream") == 1 and \
                total(values, "askai_compactions_total", by="askai") == 2 and \
                total(values, "askai_turns_stopped_total") == 1, values

            # Notes on the turns' progress, and a client that goes mid-call
            # cancels it.
            script[:] = [lambda r: (200, turn_stream([("a", "git", log)])),
                         lambda r: (200, turn_stream(text="A"), 0.5)]
            params = {"name": "ask_ai", "arguments": {"prompt": "Q"},
                      "_meta": {"progressToken": "t"}}
            with socket.create_connection(("127.0.0.1", ai.port), 10) as sock:
                sock.sendall(rpc_request(5, params, "tools/call", headers={
                    "MCP-Protocol-Version": LEGACY,
                    "Accept": "application/json, text/event-stream"}))
                events = read_events(sock)
            messages = [e["params"]["message"] for _, e in events if "method" in e]
            assert "turn 1, calling git" in messages, messages
            progress = [e["params"]["progress"] for _, e in events if "method" in e]
            assert progress == sorted(set(progress)), progress
            assert events[-1][1]["result"]["content"][0]["text"] == "A", events
            script[:] = [lambda r: (200, turn_stream([("a", "git", log)])),
                         lambda r: (200, turn_stream(text="A"), 5)]
            up.cancelled.clear()
            count = len(up.requests)
            sock = socket.create_connection(("127.0.0.1", ai.port), 10)
            sock.sendall(rpc_request(6, params, "tools/call", headers={
                "MCP-Protocol-Version": LEGACY,
                "Accept": "application/json, text/event-stream"}))
            wait_for(lambda: len(up.requests) == count + 2)
            sock.close()
            assert up.cancelled.wait(5)
            script.clear()

        # A legacy client's notifications/cancelled that comes between the
        # turns, as a tool runs, ends the call before its next turn.
        os.mkdir(f"{d}/bin")
        with open(f"{d}/bin/git", "w") as f:
            f.write(f"#!/bin/sh\ncase \"$*\" in *SLOWPATH*) sleep 1;; esac\n"
                    f"exec {shutil.which('git')} \"$@\"\n")
        os.chmod(f"{d}/bin/git", 0o755)
        slow = json.dumps({"repo": "proj", "command": "log", "paths": ["SLOWPATH"]})
        with Askai(binary, *args, env={"PATH": f"{d}/bin:" + os.environ["PATH"]}) as ai:
            for cancel in (True, False):
                script[:] = [lambda r: (200, turn_stream([("s", "git", slow)]))]
                count = len(up.requests)
                with socket.create_connection(("127.0.0.1", ai.port), 10) as sock:
                    sock.sendall(session_call(9, 77))
                    wait_for(lambda: len(up.requests) == count + 1)
                    time.sleep(0.3)
                    if cancel:
                        assert ai.rpc("notifications/cancelled", {"requestId": 77},
                                      rpc_id=None, headers={
                                          "Mcp-Session-Id": f"{9:032x}",
                                          "MCP-Protocol-Version": LEGACY})[0] == 202
                    status, body = Replies(sock).next()
                assert (status, len(up.requests) - count) == \
                    ((202, 1) if cancel else (200, 2)), (status, body)
                assert cancel or body["result"]["content"][0]["text"] == "A", body

            # A turn's calls run at once, each in a slot of its own, their
            # results and the trace's lines in the calls' order.
            script[:] = [lambda r: (200, turn_stream([(f"p{i}", "git", slow)
                                                      for i in range(3)]))]
            count = len(up.requests)
            start = time.monotonic()
            result = ai.ask("Q")
            took = time.monotonic() - start
            assert took < 2.5, took
            results = turns(up.requests[count + 1])[1:]
            assert [m["tool_call_id"] for m in results] == ["p0", "p1", "p2"], results
            assert result["content"][1]["text"].count("- git ") == 3, result

            # A job in the background, and a timer: background starts git and
            # answers at once; a wait for the job ends as it finishes, saying
            # so; jobs lists it and gives its output; a later wait ends as the
            # timer fires, saying so; a cancelled job runs no more.
            def tool_call(cid, name, **args):
                return (cid, name, json.dumps(args))
            script[:] = [
                lambda r: (200, turn_stream([
                    tool_call("b1", "background", tool="git", arguments=json.loads(slow)),
                    tool_call("t1", "timer", seconds=2, note="check it")])),
                lambda r: (200, turn_stream([tool_call("w1", "wait", seconds=30, jobs=[1])])),
                lambda r: (200, turn_stream([
                    tool_call("j1", "jobs", action="list"),
                    tool_call("j2", "jobs", action="result", job=1)])),
                lambda r: (200, turn_stream([tool_call("w2", "wait", seconds=10)])),
                lambda r: (200, turn_stream([
                    tool_call("b2", "background", tool="git", arguments=json.loads(slow)),
                    tool_call("b3", "background", tool="notes", arguments={})])),
                lambda r: (200, turn_stream([tool_call("c1", "jobs", action="cancel", job=2),
                                             tool_call("c2", "jobs", action="list"),
                                             tool_call("c3", "wait", seconds=0)])),
            ]
            count = len(up.requests)
            start = time.monotonic()
            result = ai.ask("Q")
            took = time.monotonic() - start
            assert result["content"][0]["text"] == "A", result
            asked = up.requests[count:count + 7]

            def said(k, cid):
                """What request k of the call says call cid gave."""
                return next(m["content"] for m in turns(asked[k])
                            if m.get("tool_call_id") == cid)
            assert said(1, "b1") == "Started job 1." and \
                said(1, "t1") == "Timer 1 will fire in 2 s.", turns(asked[1])
            assert said(2, "w1").startswith("[askai: job 1, git, finished: call jobs") and \
                "timer" not in said(2, "w1"), said(2, "w1")
            assert re.fullmatch(r"job 1: git \{.*\}: finished, \d+ bytes\n",
                                said(3, "j1")), said(3, "j1")
            assert said(3, "j2") == "[No output.]", said(3, "j2")
            assert said(4, "w2") == "[askai: timer 1 fired: check it.]\n", said(4, "w2")
            assert said(5, "b2") == "Started job 2." and \
                said(5, "b3").startswith("The tool argument must name a tool"), \
                turns(asked[5])
            assert said(6, "c1") == "Cancelled the job." and \
                "job 2: git" in said(6, "c2") and said(6, "c2").endswith(": cancelled\n") \
                and said(6, "c3").startswith("The seconds argument"), turns(asked[6])
            assert 2 <= took < 6, took
            trace = result["content"][1]["text"]
            assert all(f"- {t} " in trace for t in ("background", "timer", "wait", "jobs")), \
                trace

            # An answer while a timer is set, or a job runs, leaves the call
            # open: the turns doze until it fires, or ends, which a message of
            # the user's after the answer says; one that ended as they
            # answered wakes them at once.  The reply gives each answer and
            # what woke the turns after it.
            script[:] = [
                lambda r: (200, turn_stream([tool_call("t1", "timer", seconds=1,
                                                       note="look")])),
                lambda r: (200, turn_stream(text="Waiting.")),
                lambda r: (200, turn_stream([tool_call("b1", "background", tool="git",
                                                       arguments=json.loads(slow))])),
                lambda r: (time.sleep(1.5), (200, turn_stream(text="Started.")))[1],
                lambda r: (200, turn_stream(text="Done.")),
            ]
            count = len(up.requests)
            start = time.monotonic()
            result = ai.ask("Q")
            took = time.monotonic() - start
            asked = up.requests[count:]
            finished = "[askai: job 1, git, finished: call jobs with the action " \
                "result and job 1 for its output.]\n"
            assert len(asked) == 5 and turns(asked[2])[2:] == [
                {"role": "assistant", "content": "Waiting."},
                {"role": "user", "content": "[askai: timer 1 fired: look.]\n"}], asked
            assert turns(asked[4])[-2:] == [{"role": "assistant", "content": "Started."},
                                            {"role": "user", "content": finished}], asked
            assert result["content"][0]["text"] == "Waiting.\n\n[askai: timer 1 fired: " \
                "look.]\n\nStarted.\n\n" + finished + "\nDone.", result
            assert 2.4 <= took < 5, took

            # A client that takes tasks gets a task of a call that dozes, which
            # ends with the answers.
            script[:] = [lambda r: (200, turn_stream([tool_call("t1", "timer", seconds=2)])),
                         lambda r: (200, turn_stream(text="Waiting.")),
                         lambda r: (200, turn_stream(text="Done."))]
            _, _, body = ai.modern("tools/call", {
                "name": "ask_ai", "arguments": {"prompt": "Q"}}, meta=TAKES_TASKS)
            tid = body["result"]["taskId"]
            for _ in range(100):
                _, _, body = ai.modern("tasks/get", {"taskId": tid}, meta=TAKES_TASKS)
                if body["result"]["status"] != "working":
                    break
                time.sleep(0.1)
            assert body["result"]["status"] == "completed" and \
                body["result"]["result"]["content"][0]["text"] == \
                "Waiting.\n\n[askai: timer 1 fired.]\n\nDone.", body

            # A call that dozes goes as its client does, its timer with it;
            # askai stopping ends one at once, with an error.
            ask = {"name": "ask_ai", "arguments": {"prompt": "Q"}}
            for stop, secs in ((False, 1), (True, 30)):
                script[:] = [lambda r: (200, turn_stream([tool_call("t", "timer",
                                                                    seconds=secs)])),
                             lambda r: (200, turn_stream(text="Waiting."))]
                count = len(up.requests)
                with socket.create_connection(("127.0.0.1", ai.port), 10) as sock:
                    sock.sendall(rpc_request(8, ask, "tools/call"))
                    wait_for(lambda: len(up.requests) == count + 2)
                    time.sleep(0.2)
                    if stop:
                        start = time.monotonic()
                        ai.proc.send_signal(signal.SIGTERM)
                        status, body = Replies(sock).next()
                        assert status == 503 and body["error"]["message"] == \
                            "Server is shutting down", body
                        assert time.monotonic() - start < 2, time.monotonic() - start
                time.sleep(1.5)
                assert len(up.requests) == count + 2, up.requests[count:]

        # llama-server's /props tells the context, with --slots too.
        up.props = {"total_slots": 2, "default_generation_settings": {"n_ctx": 6000}}
        with Askai(binary, *args, "--slots", "1") as ai:
            ai.wait_log(r"upstream context: 6000 tokens \(llama-server's /props\)")
            assert any("upstream slots: 1 (--slots)" in line for line in ai.log), ai.log
            description = ai.rpc("tools/list")[2]["result"]["tools"][0]["description"]
            assert "It may use some of this server's tools itself" in description and \
                "It keeps notes in a thread" in description, description
            script[:] = [lambda r: (200, turn_stream([("a", "git", json.dumps(show))]))]
            count = len(up.requests)
            ai.ask("Q")
            assert "[Bytes 0 to 2992 of 44000:" in turns(up.requests[count + 1])[1][
                "content"], up.requests[count + 1]

            # A read gives the newest notes that fit a window, oldest first.
            write = lambda c: json.dumps({"action": "write", "text": c * 1200})
            script[:] = [
                lambda r: (200, turn_stream([(c, "notes", write(c)) for c in "abc"])),
                lambda r: (200, turn_stream([("d", "notes", '{"action":"read"}'),
                                             ("e", "notes", json.dumps(
                                                 {"action": "read", "search": "A"})),
                                             ("f", "notes", json.dumps(
                                                 {"action": "read", "search": "z"}))])),
            ]
            count = len(up.requests)
            ai.ask("Q")
            reads = [m["content"] for m in turns(up.requests[count + 2])[5:]]
            assert reads == [
                "[1 older note is not shown: search for it.]\n\n1. " + "b" * 1200 +
                "\n\n2. " + "c" * 1200 + "\n", "1. " + "a" * 1200 + "\n",
                "No note holds that text."], reads

            # However long the turns grow, no step does more than its share.
            work(ai)
            script[:] = [lambda r: (200, turn_stream(
                [(str(i), "git", json.dumps(show | {"offset": 3000 * i}))
                 for i in range(4)], text="t" * (3 << 20), reasoning="r" * (1 << 20)))]
            ai.ask("Q" * (512 << 10))
            units, _ = work(ai)
            assert all(most <= bound for most, bound in units.values()), units
        # /props, while it takes long, does not hold the one slot's connection.
        up.props = "hang"
        with Askai(binary, *args, "--slots", "1") as ai:
            start = time.monotonic()
            assert ai.ask("Q")["content"][0]["text"] == "A"
            assert time.monotonic() - start < 1, time.monotonic() - start
        up.props = None

        # A role that may not call git lends the turns notes and compact, and
        # wait and timer, but nothing to run in the background.
        path = f"{d}/askai.json"
        with open(path, "w") as f:
            json.dump({"roles": {"asker": {"tools": ["ask_ai"]}},
                       "anonymous": {"loopback": "asker"}}, f)
        os.chmod(path, 0o600)
        with Askai(binary, *args, "-c", path) as ai:
            count = len(up.requests)
            assert text_of(ai.ask("Q")) == "A"
            assert [t["function"]["name"] for t in up.requests[count]["body"]["tools"]] \
                == ["notes", "compact", "wait", "timer"], up.requests[count]["body"]
    up.reply = lambda request: (200, ANSWERS["openai"])
    print("turns: tools lent to the upstream, notes, threads, windows, compaction, "
          "limits: PASS")


def check_tasks(binary, up):
    """Tasks (src/task.h), through the test aid test_task, offered with
    --test-task only: its arguments checked; CPU work done a slice a step,
    placed on the client loops and spread among them, moved off a loop that
    holds too many; a call that goes cancels its task; askai stopping lets go
    of those under way; and the counts add up, in /metrics and as askai
    stops."""
    def call(ai, arguments):
        status, _, body = ai.rpc("tools/call", {"name": "test_task",
                                                "arguments": arguments})
        assert status == 200, (status, body)
        return body["result"]

    def ended(values):
        return {end: values[f'askai_tasks_ended_total{{end="{end}"}}']
                for end in ("done", "cancelled", "abandoned")}

    with Askai(binary, "-u", up.url()) as ai:
        tools = ai.rpc("tools/list")[2]["result"]["tools"]
        assert [t["name"] for t in tools] == ["ask_ai"], tools
        _, _, body = ai.rpc("tools/call", {"name": "test_task",
                                           "arguments": {"slices": 1}})
        assert body["error"]["message"] == "Unknown tool: test_task", body
    proc = subprocess.run([binary, "--test-task", "--tool-name", "test_task", "-u",
                           up.url()], capture_output=True, text=True, timeout=10)
    assert proc.returncode == 2 and "--tool-name: askai has another tool named " \
        "test_task" in proc.stderr, proc

    ai = Askai(binary, "-u", up.url(), "--test-task", loops=4)
    try:
        tools = ai.rpc("tools/list")[2]["result"]["tools"]
        assert [t["name"] for t in tools] == ["ask_ai", "test_task"], tools
        assert set(tools[1]["inputSchema"]["properties"]) == {"slices", "prompt"}, \
            tools[1]
        slices = "The slices argument must be a whole number from 1 to 100000."
        for arguments, text in ((["x"], "Tool arguments must be an object."),
                                ({}, slices), ({"slices": 0}, slices),
                                ({"slices": 100001}, slices), ({"slices": "3"}, slices),
                                ({"slices": 1.5}, slices),
                                ({"slices": 1, "prompt": "Q"},
                                 "Give slices or a prompt, not both."),
                                ({"prompt": ""},
                                 "The prompt argument must be a non-empty string.")):
            result = call(ai, arguments)
            assert result.get("isError") and text_of(result) == text, (arguments, result)
        assert text_of(call(ai, {"slices": 3})) == \
            "Ran 3 slices, and moved 0 times between loops.", ai.log

        # Calls of mixed lengths at once spread their tasks over every loop,
        # and a loop left holding more than eight moves some off.
        moves = 0
        rounds = 0
        while not moves and rounds < 5:
            rounds += 1
            texts = []
            threads = [threading.Thread(target=lambda n=n: texts.append(
                text_of(call(ai, {"slices": n})))) for n in (8, 400) * 20]
            for thread in threads:
                thread.start()
            for thread in threads:
                thread.join(30)
            assert len(texts) == 40, texts
            moves += sum(int(re.fullmatch(r"Ran \d+ slices, and moved (\d+) times "
                                          r"between loops\.", t).group(1)) for t in texts)
        values = ai.metrics()
        ran = [v for k, v in values.items() if k.startswith("askai_task_slices_total{")]
        assert len(ran) == 4 and min(ran) > 0, values
        assert sum(v for k, v in values.items()
                   if k.startswith("askai_tasks_moved_total{")) == moves > 0, values
        assert values["askai_tasks_submitted_total"] == ended(values)["done"], values
        assert not any(v for k, v in values.items() if k.startswith("askai_tasks_held{"))
        assert all(v == 1 for k, v in values.items()
                   if k.startswith("askai_loop_step_work_max{") and 'unit="tasks"' in k
                   and 'loop="home"' not in k), values

        # A call that goes cancels its task, which goes at its next slice.
        before = ended(ai.metrics())
        with socket.create_connection(("127.0.0.1", ai.port), 10) as sock:
            sock.sendall(rpc_request(7, {"name": "test_task",
                                         "arguments": {"slices": 100000}}, "tools/call"))
            wait_for(lambda: sum(v for k, v in ai.metrics().items()
                                 if k.startswith("askai_tasks_held{")) == 1)
        wait_for(lambda: ended(ai.metrics())["cancelled"] == before["cancelled"] + 1)
        assert not any(v for k, v in ai.metrics().items()
                       if k.startswith("askai_tasks_held{"))

        # Stopping ends a call under way, and lets go of its task.
        with socket.create_connection(("127.0.0.1", ai.port), 10) as sock:
            sock.sendall(rpc_request(8, {"name": "test_task",
                                         "arguments": {"slices": 100000}}, "tools/call"))
            wait_for(lambda: sum(v for k, v in ai.metrics().items()
                                 if k.startswith("askai_tasks_held{")) == 1)
            ai.stop()
            status, body = Replies(sock).next()
            assert status == 503 and body["error"]["message"] == \
                "Server is shutting down", (status, body)
    except BaseException:
        ai.proc.kill()
        ai.proc.wait()
        sys.stderr.write("".join(ai.log))
        raise
    line = next(l for l in ai.log if l.startswith("askai: tasks: "))
    counts = [int(n) for n in re.findall(r"\d+", line)]
    assert len(counts) == 5 and counts[0] == sum(counts[1:4]) == 3 + 40 * rounds \
        and counts[4] == moves, line
    print("tasks: test_task with --test-task only, arguments, spread over the "
          "loops, moved, cancelled, stopped, counted: PASS")


def check_plugin(binary, up, plugin):
    """askai loads the plugins that the configuration file lists.

    The test plugin (src/test_plugin.c) has two tools.  The check uses the
    first one, test_echo.  The check lists and calls test_echo, grants it to
    a role by name, reloads the configuration, calls it from a turn of an
    ask call, and then starts askai with files that askai must refuse.
    """
    plugin = os.path.realpath(plugin)
    with open(plugin, "rb") as f:
        image = f.read()
    # The manifest is an ELF note (src/manifest.h): the sizes of the owner
    # and of the manifest, the type, the owner "askai", then the manifest.
    # The manifest starts with the epoch and the ABI values, 4 bytes each.
    head = b"".join(n.to_bytes(4, sys.byteorder)
                    for n in (6, 548, 0x61736B01)) + b"askai\0\0\0"
    assert image.count(head) == 1, image.count(head)
    at = image.index(head) + len(head)

    def word(index):
        return int.from_bytes(image[at + 4 * index:at + 4 * index + 4],
                              sys.byteorder)

    def number(offset, size):
        return int.from_bytes(image[offset:offset + size], sys.byteorder)

    def segment(kind):
        """Returns the file offset of the program header of type kind."""
        assert image[4] == 2, "the plugin is not an ELF64 file"
        first, size, count = number(32, 8), number(54, 2), number(56, 2)
        for offset in range(first, first + size * count, size):
            if number(offset, 4) == kind:
                return offset
        raise AssertionError(f"no program header of type {kind:#x}")

    def dynamic(tag):
        """Returns the file offset of the low 4 bytes of the tag of the
        dynamic section's entry whose tag is tag."""
        header = segment(2)
        first, size = number(header + 8, 8), number(header + 32, 8)
        for offset in range(first, first + size, 16):
            if number(offset, 8) == tag:
                return offset + (4 if sys.byteorder == "big" else 0)
        raise AssertionError(f"no dynamic entry with tag {tag:#x}")

    def symbol(name):
        """Returns the file offset of the dynamic symbol name.  The section
        header of type SHT_DYNSYM (11) gives the symbols, and its sh_link
        names the section of their names.  A PT_LOAD program header (1)
        maps the symbol's address to its file offset."""
        first, size, count = number(40, 8), number(58, 2), number(60, 2)
        for header in range(first, first + size * count, size):
            if number(header + 4, 4) != 11:
                continue
            names = number(first + size * number(header + 40, 4) + 24, 8)
            start, end = number(header + 24, 8), number(header + 24, 8) + number(header + 32, 8)
            for sym in range(start, end, number(header + 56, 8)):
                at_name = names + number(sym, 4)
                if image[at_name:image.index(b"\0", at_name)] == name:
                    address = number(sym + 8, 8)
                    break
            else:
                continue
            break
        else:
            raise AssertionError(f"no dynamic symbol {name}")
        first, size, count = number(32, 8), number(54, 2), number(56, 2)
        for header in range(first, first + size * count, size):
            vaddr = number(header + 16, 8)
            if number(header, 4) == 1 and vaddr <= address < vaddr + number(header + 32, 8):
                return number(header + 8, 8) + address - vaddr
        raise AssertionError(f"{name} is not in the file")

    # askai that is built with -fsanitize=address, -fsanitize=thread or
    # -fsanitize=undefined refuses no plugin file for thread-local storage,
    # constructors or writable data (src/plugin.c).  The check recognizes
    # such a binary by the names of the sanitizer's functions.  A binary
    # that is built with -fsanitize-trap=undefined has none of these names,
    # so the check does not work with that build.
    with open(binary, "rb") as f:
        text = f.read()
    refuses_state = not re.search(rb"__(asan|tsan)_init|__ubsan_handle", text)

    with tempfile.TemporaryDirectory() as d:
        d = os.path.realpath(d)
        path = f"{d}/askai.json"

        def write(plugins, **options):
            text = json.dumps({"url": up.url(), "man": True,
                               "plugins": plugins} | options)
            with open(f"{path}.new", "w") as f:
                f.write(text)
            os.chmod(f"{path}.new", 0o600)
            os.rename(f"{path}.new", path)

        def call(ai, name, arguments):
            status, _, body = ai.rpc("tools/call",
                                     {"name": name, "arguments": arguments})
            assert status == 200, (status, body)
            return body

        def names(ai):
            return [t["name"] for t in ai.rpc("tools/list")[2]["result"]["tools"]]

        def fails(plugins, *messages, args=()):
            write(plugins)
            run = subprocess.run([binary, "-l", "127.0.0.1:0", "-c", path, *args],
                                 stdin=subprocess.DEVNULL, capture_output=True,
                                 text=True, timeout=5, check=False)
            assert run.returncode == 2, (plugins, run.returncode, run.stderr)
            for message in messages:
                assert message in run.stderr, (message, run.stderr)

        def patched(name, index, value):
            """Writes a copy of the plugin in which the 4-byte word at index
            is value, and returns its path.  Index 0 is the first word of
            the manifest.  Index -3 is the type of the note."""
            copy = bytearray(image)
            copy[at + 4 * index:at + 4 * index + 4] = \
                value.to_bytes(4, sys.byteorder)
            with open(f"{d}/{name}", "wb") as f:
                f.write(copy)
            return f"{d}/{name}"

        # Without roles every client may call the plugin's tool.  Its id
        # follows those of askai's own tools.
        write([plugin])
        with Askai(binary, "-c", path) as ai:
            tools = ai.rpc("tools/list")[2]["result"]["tools"]
            assert [t["name"] for t in tools] == \
                ["ask_ai", "man", "test_echo", "test_state"], tools
            assert tools[2]["description"] == "Returns the text argument unchanged." \
                and tools[2]["inputSchema"]["required"] == ["text"], tools[2]
            for text in ("plugin \u20ac", "", "x" * 100000):
                result = call(ai, "test_echo", {"text": text})["result"]
                assert not result.get("isError") and text_of(result) == text, \
                    (len(text), result)
            for arguments in ({"text": 5}, {}):
                result = call(ai, "test_echo", arguments)["result"]
                assert result.get("isError") and text_of(result) == \
                    "The text argument must be a string.", (arguments, result)
            assert call(ai, "test_none", {})["error"]["message"] == \
                "Unknown tool: test_none"
            # A call of askai's own man tool returns a result while the plugin
            # is loaded.  The text depends on the installed man and its pages.
            assert "result" in call(ai, "man", {"name": "ls"})

        # A role grants the tool by its name.  A reload reads the names
        # from the registry that askai started with.
        only = {"roles": {"echo": {"tools": ["test_echo"]}},
                "anonymous": {"loopback": "echo"}}
        write([plugin], **only)
        with Askai(binary, "-c", path) as ai:
            assert names(ai) == ["test_echo"]
            assert text_of(call(ai, "test_echo", {"text": "a"})["result"]) == "a"
            assert call(ai, "man", {"name": "ls"})["error"]["message"] == \
                "Tool not available: man"
            write([plugin], roles={"both": {"tools": ["man", "test_echo"]}},
                  anonymous={"loopback": "both"})
            ai.log.clear()
            ai.proc.send_signal(signal.SIGHUP)
            line = ai.wait_log("configuration reloaded")
            assert not line.strip().endswith("take a restart"), line
            assert names(ai) == ["man", "test_echo"]
            assert text_of(call(ai, "test_echo", {"text": "b"})["result"]) == "b"
            # A changed list of plugins takes a restart: the tool stays.
            write([], **only)
            ai.log.clear()
            ai.proc.send_signal(signal.SIGHUP)
            line = ai.wait_log("configuration reloaded")
            assert line.strip().endswith("its other options take a restart"), line
            assert names(ai) == ["test_echo"]

        # With "upstream-tools" the registry also has the tools that only
        # the turns of an ask call use.  registry_add() inserts test_echo
        # before them, which increases each of their ids by one.  The kind
        # of test_echo has TOOL_LENT, so askai sends its definition to the
        # upstream with those tools.  The first turn calls timer, whose id
        # changed, and test_echo.  askai counts the call of test_echo in
        # askai_turn_tool_calls_total{tool="plugin"}.
        write([plugin], **{"upstream-tools": True})
        up.reply = lambda request: (200, turn_stream(
            [("a", "timer", "{}"), ("b", "test_echo", '{"text":"x"}')]
            if len(request["body"]["messages"]) == 1 else (), text="A"))
        with Askai(binary, "-c", path) as ai:
            assert names(ai) == ["ask_ai", "man", "test_echo", "test_state"]
            count = len(up.requests)
            answer, trace = [c["text"] for c in ai.ask("Q")["content"]]
            first, second = up.requests[count:]
            assert [t["function"]["name"] for t in first["body"]["tools"]] == \
                ["man", "test_echo", "test_state", "compact", "background",
                 "jobs", "wait", "timer"], first
            assert [m["content"] for m in second["body"]["messages"][2:]] == \
                ["The seconds argument must be a whole number from 1 to 3600.",
                 "x"], second
            assert answer == "A" and '- test_echo {"text":"x"}: 1 bytes\n' in trace, \
                trace
            assert total(ai.metrics(), "askai_turn_tool_calls_total",
                         tool="plugin") == 1
        up.reply = lambda request: (200, ANSWERS["openai"])

        # Every refusal names the file and stops askai as it starts.
        with open(f"{d}/text.so", "w") as f:
            f.write("not a shared object\n")
        with open(f"{d}/short.so", "wb") as f:
            f.write(image[:at + 100])
        os.mkdir(f"{d}/dir.so")
        for listed, message in (
                (f"{d}/none.so", "cannot open it: No such file or directory"),
                (f"{d}/text.so", "not a shared object of this machine"),
                (f"{d}/dir.so", "not a regular file"),
                (f"{d}/short.so", "a truncated or malformed ELF file"),
                (patched("type.so", -3, 0), "has no askai manifest"),
                (patched("epoch.so", 0, word(0) + 1),
                 f"its plugin epoch is {word(0) + 1}, askai's is {word(0)}"),
                (patched("abi.so", 1, word(1) + 1),
                 f"its ABI value 0 is {word(1) + 1}, askai's is {word(1)}"),
                (patched("probes.so", 32, 1), "its ABI value 31 is 1, askai's is 0")):
            fails([listed], f"askai: {listed}: {message}")
        # A copy that calls mcp_slot_tel1(), which askai does not export.
        with open(f"{d}/import.so", "wb") as f:
            assert b"mcp_slot_tell\0" in image
            f.write(image.replace(b"mcp_slot_tell\0", b"mcp_slot_tel1\0"))
        fails([f"{d}/import.so"], f"askai: {d}/import.so: cannot load it: ",
              "undefined symbol: mcp_slot_tel1")
        # A copy in which kind 0 of askai_plugin has the flag 1 << 32, which
        # src/tool.h does not define for plugins.  The manifest's ABI value
        # 6 is the offset of the member flags in struct tool_kind.
        flags = symbol(b"askai_plugin") + word(7)
        copy = bytearray(image)
        copy[flags:flags + 8] = (number(flags, 8) | 1 << 32).to_bytes(8, sys.byteorder)
        with open(f"{d}/flags.so", "wb") as f:
            f.write(copy)
        fails([f"{d}/flags.so"], f"askai: {d}/flags.so: kind 0 of its askai_plugin "
              "has flags that tool.h does not define for plugins: 0x100000000")
        # A copy whose definition of test_echo has the name test_ech0.
        assert image.count(b'{"name":"test_echo",') == 1
        with open(f"{d}/name.so", "wb") as f:
            f.write(image.replace(b'{"name":"test_echo",', b'{"name":"test_ech0",'))
        fails([f"{d}/name.so"], f"askai: {d}/name.so: the definition of its tool "
              "test_echo has another name")
        # Only root and askai's user may change a plugin file and the
        # directories above it.  The path of a plugin is the file's real
        # path.
        shutil.copy(plugin, f"{d}/open.so")
        os.chmod(f"{d}/open.so", 0o666)
        os.mkdir(f"{d}/shared")
        os.chmod(f"{d}/shared", 0o777)
        shutil.copy(plugin, f"{d}/shared/test.so")
        os.symlink(plugin, f"{d}/link.so")
        for listed in (f"{d}/open.so", f"{d}/shared/test.so"):
            fails([listed], f"askai: {listed}: others than root and askai's "
                  "user may change it")
        for listed in (f"{d}/link.so", f"{d}/shared/../import.so",
                       f"{os.path.dirname(plugin)}/./{os.path.basename(plugin)}"):
            fails([listed], f"askai: {listed}: not the file's real path, which "
                  f"is {os.path.realpath(listed)}")
        fails([f"{d}/none.so"], f"askai: {d}/none.so: cannot open it: No such "
              "file or directory")
        # A plugin's state is in its instances.  askai refuses a file with
        # thread-local storage, with a constructor, or with writable data
        # that the dynamic linker does not make read-only.  Each copy has
        # one changed word: the type of the PT_GNU_RELRO program header
        # (0x6474e552) becomes PT_TLS (7) or PT_NULL (0), and the tag
        # DT_FLAGS_1 (0x6ffffffb) of the dynamic section becomes DT_INIT
        # (12).
        relro = (segment(0x6474E552) - at) // 4
        for name, index, value, message in (
                ("tls.so", relro, 7, "has thread-local storage"),
                ("relro.so", relro, 0, "has writable data"),
                ("init.so", (dynamic(0x6FFFFFFB) - at) // 4, 12,
                 "has a constructor or a destructor")):
            listed = patched(name, index, value)
            if refuses_state:
                fails([listed], f"askai: {listed}: {message}")
        fails([plugin, plugin], f"askai: {plugin}: its tool test_echo has the "
              "name of a tool that askai already has")
        fails([plugin], f"askai: {plugin}: its tool test_echo has the name of a "
              "tool that askai already has", args=("--tool-name", "test_echo"))
        fails(["test_plugin.so"], f"askai: {path}: plugins: not an absolute "
              "path: test_plugin.so")
        fails([5], f"askai: {path}: plugins: not an absolute path: 5")
        fails(plugin, f"askai: {path}: plugins: not an array")
        # A role cannot name a tool of a plugin that is not loaded.
        write([], **only)
        run = subprocess.run([binary, "-l", "127.0.0.1:0", "-c", path],
                             stdin=subprocess.DEVNULL, capture_output=True,
                             text=True, timeout=5, check=False)
        assert run.returncode == 2 and \
            'askai offers no tool "test_echo"' in run.stderr, run.stderr

    # "plugins" is a member of the configuration file, not an option.
    run = subprocess.run([binary, "-u", up.url(), "--plugins", plugin],
                         stdin=subprocess.DEVNULL, capture_output=True,
                         text=True, timeout=5, check=False)
    assert run.returncode == 2, (run.returncode, run.stderr)
    print("plugin: test_echo listed, called, granted by a role, kept over a "
          "reload and called from a turn; files that are no plugin of this "
          "askai refused: PASS")


def check_instances(binary, up, plugin):
    """A plugin has one instance for each user on each client loop.

    The tool test_state of the test plugin (src/test_plugin.c) answers with
    the principal of the call's user, the address of the call's instance,
    the number of calls that the instance has had, and the text that the
    instance stores.  A call without "wait_ms" stores its "text" argument in
    the instance after it has answered.  A call with "wait_ms" stores it
    when it starts, and the drop() of such a call stores "dropped".  The
    check uses these values to show that two users never get the same
    instance, on one loop and on four, that a turn of a user's ask call gets
    the user's instance, and when askai destroys an instance.
    """
    plugin = os.path.realpath(plugin)

    def digest(token):
        return "sha256:" + hashlib.sha256(token.encode()).hexdigest()

    def auth(token):
        return {"Authorization": f"Bearer {token}"} if token else {}

    def state(ai, token, conn=None, **arguments):
        """Calls test_state as the identity that has the token.  Returns
        the four values of the answer.  They are the principal, the address
        of the instance, the number of calls of the instance, and the text
        that was in the instance when the call ended."""
        status, _, body = ai.rpc(
            "tools/call", {"name": "test_state", "arguments": arguments},
            headers=auth(token), conn=conn)
        assert status == 200 and not body["result"].get("isError"), (status, body)
        found = re.fullmatch(r"principal=(\w+) instance=(\S+) calls=(\d+) last=(.*)",
                             text_of(body["result"]), re.S)
        assert found, body
        return found[1], found[2], int(found[3]), found[4]

    def parked(ai, token, ms):
        """Starts a call of test_state that waits ms milliseconds.  The
        call runs in a new thread.  The function returns when askai has
        started the call.  It returns the thread, the list that the thread
        appends the call's answer to, and the number of calls that the
        user's instance has had by then.  askai must run with one loop."""
        answer = []
        thread = threading.Thread(target=lambda: answer.append(
            state(ai, token, text="held", wait_ms=ms)))
        calls = state(ai, token, text="poll")[2]
        thread.start()
        # The waiting call adds 1 to the instance's number of calls when
        # it starts.  Each call of this loop adds 1 too.  So the waiting
        # call has started when the number has grown by 2 between two
        # calls of this loop.
        end = time.monotonic() + 5
        while True:
            now = state(ai, token, text="poll")[2]
            if now == calls + 2:
                return thread, answer, now
            assert now == calls + 1 and time.monotonic() < end, (calls, now)
            calls = now

    roles = {"all": {"tools": ["ask_ai", "test_state"]}}
    users = {name: {"role": "all", "tokens": [digest(name)]}
             for name in ("alice", "bob")}
    hold = up.reply
    with tempfile.TemporaryDirectory() as d:
        path = f"{d}/askai.json"

        def write(**options):
            text = json.dumps({"url": up.url(), "plugins": [plugin]} | options)
            with open(f"{path}.new", "w") as f:
                f.write(text)
            os.chmod(f"{path}.new", 0o600)
            os.rename(f"{path}.new", path)

        # One loop.  Each user has an instance of its own, and gets the
        # same instance again from a new connection.
        write(roles=roles, identities=users, **{"upstream-tools": True})
        with Askai(binary, "-c", path, loops=1) as ai:
            alice, hers, calls, last = state(ai, "alice", text="A-secret")
            assert (calls, last) == (1, ""), (calls, last)
            bob, his, calls, last = state(ai, "bob", text="B-secret")
            assert (calls, last) == (1, "") and bob != alice and his != hers, \
                (calls, last, alice, bob, hers, his)
            assert state(ai, "alice", text="A2") == (alice, hers, 2, "A-secret")
            assert state(ai, "bob") == (bob, his, 2, "B-secret")
            # Both users send their requests on one connection that stays
            # open, as happens when a proxy forwards them.
            conn = ai.connect()
            assert state(ai, "alice", conn=conn) == (alice, hers, 3, "A2")
            assert state(ai, "bob", conn=conn) == (bob, his, 3, "")
            conn.close()

            # A turn of alice's ask call calls the tool.  The call gets
            # alice's instance, not a separate one.
            up.reply = lambda request: (200, turn_stream(
                [("a", "test_state", '{"text":"turn"}')]
                if len(request["body"]["messages"]) == 1 else (), text="A"))
            count = len(up.requests)
            status, _, body = ai.rpc(
                "tools/call", {"name": "ask_ai", "arguments": {"prompt": "Q"}},
                headers=auth("alice"))
            assert status == 200 and not body["result"].get("isError"), (status, body)
            second = up.requests[count:][1]
            assert second["body"]["messages"][2]["content"] == \
                f"principal={alice} instance={hers} calls=4 last=", second
            up.reply = hold
            assert state(ai, "alice") == (alice, hers, 5, "turn")
            assert state(ai, "bob") == (bob, his, 4, "")

            # A reload.  The call that waits started before the reload.
            # It has the old instance until it ends.  A call that starts
            # after the reload gets a new instance, and later calls get
            # that new instance too.
            waiter, held, calls = parked(ai, "alice", 1500)
            write(roles=roles, identities=users | {
                "carol": {"role": "all", "tokens": [digest("carol")]}},
                **{"upstream-tools": True})
            ai.log.clear()
            ai.proc.send_signal(signal.SIGHUP)
            ai.wait_log("configuration reloaded")
            after = state(ai, "alice", text="after")
            assert after[0] == alice and after[2:] == (1, ""), after
            assert state(ai, "bob")[2:] == (1, "")
            waiter.join()
            assert held == [(alice, hers, calls, "poll")] and after[1] != hers, \
                (held, after)
            assert state(ai, "alice")[1:] == (after[1], 2, "after")
            assert state(ai, "carol")[2:] == (1, "")

            # An ask call of alice starts before a reload, and its turn
            # calls test_state after the reload.  The request of the ask
            # call began before the reload, so the turn's call gets a new
            # instance that no other call gets.  It does not get the
            # instance that alice's requests get after the reload.
            go = threading.Event()

            def late(request):
                first = len(request["body"]["messages"]) == 1
                assert not first or go.wait(5)
                return 200, turn_stream(
                    [("a", "test_state", '{"text":"late"}')] if first else (),
                    text="A")

            up.reply = late
            up.arrived.clear()
            count = len(up.requests)
            asked = []
            asker = threading.Thread(target=lambda: asked.append(ai.rpc(
                "tools/call", {"name": "ask_ai", "arguments": {"prompt": "Q"}},
                headers=auth("alice"))))
            asker.start()
            assert up.arrived.wait(5)
            ai.log.clear()
            ai.proc.send_signal(signal.SIGHUP)
            ai.wait_log("configuration reloaded")
            mine = state(ai, "alice", text="mine")
            assert mine[2:] == (1, ""), mine
            go.set()
            asker.join()
            up.reply = hold
            status, _, body = asked[0]
            assert status == 200 and not body["result"].get("isError"), (status, body)
            told = up.requests[count:][1]["body"]["messages"][2]["content"]
            assert re.fullmatch(rf"principal={alice} instance=\S+ calls=1 last=",
                                told), told
            assert state(ai, "alice")[1:] == (mine[1], 2, "mine")

        # The policy has no identities, so every TCP client is anonymous.
        # Each call of an anonymous client gets a new instance.  A call
        # adds 1 to the number of calls of its instance when it starts, so
        # the number is 1 in every answer only if no two of the calls that
        # wait got the same instance.
        write()
        with Askai(binary, "-c", path, loops=1) as ai:
            conn = ai.connect()
            seen = [state(ai, None, conn=conn, text=f"t{k}") for k in range(5)]
            conn.close()
            assert all(principal == "0" * 16 and (calls, last) == (1, "")
                       for principal, _, calls, last in seen), seen
            waited = []
            threads = [threading.Thread(target=lambda: waited.append(
                state(ai, None, wait_ms=300))) for _ in range(100)]
            for thread in threads:
                thread.start()
            for thread in threads:
                thread.join()
            assert len(waited) == 100 and all(w[2] == 1 for w in waited), waited

        # More users than a loop's table has room for (INSTANCE_MAX is 256,
        # src/instance.h).  The table destroys the instance that has been
        # unused for the longest time, so the last 256 users get their
        # instances again and the users before them get new ones.
        many = {f"u{k}": {"role": "all", "tokens": [digest(f"u{k}")]}
                for k in range(400)}
        write(roles=roles, identities=many)
        with Askai(binary, "-c", path, "--max-client-conns", "0", loops=1) as ai:
            for k in range(400):
                assert state(ai, f"u{k}", text=f"s{k}")[2] == 1
            assert state(ai, "u399")[2:] == (2, "s399")
            assert state(ai, "u144")[2:] == (2, "s144")
            assert state(ai, "u143")[2:] == (1, "")
            assert state(ai, "u0")[2:] == (1, "")

            # Each of 256 users has a call that waits, so calls use all 256
            # instances of the table and the table cannot destroy one.  The
            # call of another user then ends as failed, and the client can
            # repeat it.  When the 256 clients close their connections,
            # askai drops their calls, and the call of the other user
            # succeeds.
            held = []
            for k in range(256):
                sock = socket.create_connection((ai.host, ai.port), 10)
                sock.sendall(rpc_request(
                    1, {"name": "test_state", "arguments": {"wait_ms": 60000}},
                    "tools/call", headers=auth(f"u{k}")))
                held.append(sock)

            def refused():
                status, _, body = ai.rpc(
                    "tools/call", {"name": "test_state", "arguments": {}},
                    headers=auth("u256"))
                assert status == 200 and "result" in body, (status, body)
                return body["result"].get("isError") and text_of(body["result"]) == \
                    "Too many tool calls are under way; try again in a few seconds."

            wait_for(refused)
            for sock in held:
                sock.close()
            wait_for(lambda: not refused())

        # Four loops, two connections at a time for each user.  A user has
        # at most one instance on each loop, and every answer has only that
        # user's own text.
        write(roles=roles, identities=users)
        with Askai(binary, "-c", path, loops=4) as ai:
            seen = {"alice": [], "bob": []}

            def calls_of(name):
                for k in range(200):
                    seen[name].append(state(ai, name, text=f"{name}-{k}"))

            threads = [threading.Thread(target=calls_of, args=(name,))
                       for name in seen for _ in range(2)]
            for thread in threads:
                thread.start()
            for thread in threads:
                thread.join()
            for name, other in (("alice", "bob"), ("bob", "alice")):
                assert len(seen[name]) == 400, len(seen[name])
                instances = {s[1] for s in seen[name]}
                assert len({s[0] for s in seen[name]}) == 1 and \
                    1 <= len(instances) <= 4, (name, instances)
                assert all(s[3] == "" or s[3].startswith(f"{name}-")
                           for s in seen[name]), name
                assert not instances & {s[1] for s in seen[other]}, name

        # Memory pressure.  At the critical level askai destroys every
        # instance that no call uses.  The call that waits uses alice's
        # instance, so that instance stays, and the call ends with it.
        level = tempfile.NamedTemporaryFile("w", prefix="askai-pressure-")
        set_pressure(level, 0)
        with Askai(binary, "-c", path, "--test-pressure", level.name, loops=1) as ai:
            alice, hers = state(ai, "alice")[:2]
            for _ in range(5):
                state(ai, "bob", text="y")
            waiter, held, calls = parked(ai, "alice", 4000)
            set_pressure(level, 2)
            wait_for(lambda: state(ai, "bob")[2] == 1, seconds=3.5)
            set_pressure(level, 0)
            waiter.join()
            assert held == [(alice, hers, calls, "poll")], held
            # The drop() of the call that waited stored "dropped" in alice's
            # instance.  This shows that mcp_slot_instance() returned the
            # instance inside drop().
            assert state(ai, "alice") == (alice, hers, calls + 1, "dropped")
    print("instances: one for each user on a loop, also from a turn; a new "
          "one for each anonymous call, after a reload, past 256 users and "
          "under memory pressure; a failed call when calls use all 256; four "
          "loops: PASS")


def check_classes(binary, up):
    """The LLM scheduler's classes (S8): test_task's prompt asks as
    background work, which interactive calls go before.  With two slots or
    more, background jobs hold all but one, which stays free for interactive
    ones; with one, an interactive job that has to wait preempts a
    background one, whose upstream call is cut and which starts again
    later, preempted once at most."""
    def background(ai, prompt, out):
        status, _, body = ai.rpc("tools/call", {"name": "test_task",
                                                "arguments": {"prompt": prompt}})
        out.append((prompt, status, body))

    def timed(ai, prompt):
        start = time.monotonic()
        status, _, body = ai.rpc("tools/call", {"name": "ask_ai",
                                                "arguments": {"prompt": prompt}})
        assert status == 200 and text_of(body["result"]) == "A", body
        return time.monotonic() - start

    def prompts():
        with up.lock:
            return [r["body"]["messages"][-1]["content"] for r in up.requests]

    hold = up.reply
    up.reply = lambda request: (200, ANSWERS["openai"], 1.5 if b"bg" in
                                request["raw"] else 0.05)
    try:
        # Two slots: the background jobs take one, and wait for it.
        with Askai(binary, "-u", up.url(), "--test-task", "--slots", "2") as ai:
            out = []
            threads = [threading.Thread(target=background, args=(ai, f"bg{i}", out))
                       for i in range(3)]
            for thread in threads:
                thread.start()
            wait_for(lambda: ai.metrics()["askai_jobs_queued"] == 2)
            values = ai.metrics()
            assert values["askai_upstream_background_calls"] == 1 and \
                values["askai_upstream_calls"] == 1, values
            assert timed(ai, "fg") < 1, "an interactive call waited"
            for thread in threads:
                thread.join(10)
            assert len(out) == 3 and all(s == 200 and text_of(b["result"]) == "A"
                                         for _, s, b in out), out
            assert ai.metrics()["askai_jobs_preempted_total"] == 0

        # One slot: an interactive job preempts the background one running,
        # which starts again, and is not preempted again.
        del up.requests[:]
        hangups = up.hangups
        with Askai(binary, "-u", up.url(), "--test-task", "--slots", "1") as ai:
            out = []
            thread = threading.Thread(target=background, args=(ai, "bg", out))
            thread.start()
            wait_for(lambda: ai.metrics()["askai_upstream_background_calls"] == 1)
            assert timed(ai, "fg1") < 1, "the interactive call was not first"
            wait_for(lambda: prompts().count("bg") == 2)
            assert up.hangups == hangups + 1, (up.hangups, hangups)
            assert ai.metrics()["askai_jobs_preempted_total"] == 1
            took = timed(ai, "fg2")
            assert took > 0.5, f"preempted twice: the call took {took:.2f} s"
            thread.join(10)
            assert len(out) == 1 and out[0][1] == 200 and \
                text_of(out[0][2]["result"]) == "A", out
            assert prompts() == ["bg", "fg1", "bg", "fg2"], prompts()
            values = ai.metrics()
            assert values["askai_jobs_preempted_total"] == 1 and \
                values["askai_upstream_cancels_total"] == 1, values
        line = next(l for l in ai.log if "jobs: " in l)
        assert line.rstrip().endswith("; 1 cancelled upstream; 1 preempted"), line
    finally:
        up.reply = hold
    print("classes: background work from test_task's prompt, a slot kept for "
          "interactive work, preemption with one slot, once: PASS")


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else "./askai"
    # The test plugin is built beside askai (tests/Makefile).
    plugin = sys.argv[2] if len(sys.argv) > 2 else \
        os.path.join(os.path.dirname(binary), "test_plugin.so")
    up = Upstream()
    try:
        for check in (check_legacy_session, check_protocols, check_upstream_errors,
                      check_modern, check_mcp_tasks, check_http, check_large_bodies,
                      check_sse,
                      check_answers, check_dense, check_connections,
                      check_cancellation, check_limits, check_loops, check_turn_order,
                      check_isolation, check_configuration, check_identities,
                      check_reload, check_unix, check_tls, check_metrics,
                      check_memory, check_lookups, check_jobs, check_streaming,
                      check_scheduler, check_breaker, check_slots, check_progress,
                      check_man, check_git, check_turns, check_turn_apis, check_tasks,
                      lambda binary, up: check_plugin(binary, up, plugin),
                      lambda binary, up: check_instances(binary, up, plugin),
                      check_classes,
                      check_shutdown,
                      check_watchdog, check_stalled_stderr):
            check(binary, up)
    finally:
        up.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
