#!/usr/bin/env python3
"""End-to-end checks for the askai MCP server against fake AI endpoints."""

import base64
import errno
import fcntl
import gzip
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
import urllib.parse
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
       "gemini": "generate", "interactions": "interactions"}

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
    "interactions": {"status": "completed", "steps": [
        {"type": "thought", "signature": "sig",
         "summary": [{"type": "text", "text": "hmm"}]},
        {"type": "model_output", "content": [{"type": "text", "text": "A"}]}],
        "usage": {"total_input_tokens": 10, "total_output_tokens": 1,
                  "total_thought_tokens": 5, "total_tokens": 16}},
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
    "interactions": {"status": "incomplete", "steps": [
        {"type": "model_output", "content": [{"type": "text", "text": "A"}]}]},
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
    elif "steps" in payload:
        # Gemini's Interactions sends each step as a start, deltas and a
        # stop, and then the end without the steps.
        def event(name, **data):
            events.append((name, {"event_type": name} | data))

        event("interaction.created", interaction={"id": "v1_x",
                                                  "status": "in_progress"})
        for i, step in enumerate(payload["steps"]):
            kind = step["type"]
            start = {"type": kind}
            deltas = []
            if kind == "thought":
                start["signature"] = ""
                deltas = [{"type": "thought_summary", "content": item}
                          for item in step.get("summary", [])]
                if step.get("signature"):
                    deltas.append({"type": "thought_signature",
                                   "signature": step["signature"]})
            elif kind == "model_output":
                deltas = [{"type": "text", "text": piece}
                          for item in step.get("content", [])
                          for piece in pieces(item["text"])]
            elif kind == "function_call":
                start |= {"id": step["id"], "name": step["name"], "arguments": {}}
                deltas = [{"type": "arguments_delta", "arguments": piece}
                          for piece in pieces(json.dumps(step["arguments"]))]
            event("step.start", index=i, step=start)
            for delta in deltas:
                event("step.delta", index=i, delta=delta)
            event("step.stop", index=i)
        event("interaction.completed",
              interaction={k: v for k, v in payload.items() if k != "steps"})
        events.append(("done", "[DONE]"))
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
    signals ignored; with default, with those signals at their default
    action; with data, with that data segment limit."""

    def __init__(self, binary, *args, loops=4, env=None, nofile=None,
                 preload=None, ignore=(), default=(), data=None):
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
            for sig in default:
                signal.signal(sig, signal.SIG_DFL)

        if loops is not None:
            args = ("--loops", str(loops), *args)
        self.proc = subprocess.Popen(
            [binary, "-l", "127.0.0.1:0", *args], env=child_env,
            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE, text=True,
            preexec_fn=setup if nofile or ignore or default or data else None)
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

    def listed(self, header):
        """The items of the list under the first line @p header of the log,
        without the prefix and the indent, or None without that line."""
        lines = [line.removeprefix("askai: ").rstrip("\n") for line in list(self.log)]
        if header not in lines:
            return None
        items = []
        for line in lines[lines.index(header) + 1:]:
            if not line.startswith("  "):
                break
            items.append(line[2:])
        return items

    def wait_list(self, header, items, timeout=5):
        """Wait for the list @p items under the line @p header in the log."""
        end = time.monotonic() + timeout
        while (got := self.listed(header)) != items and time.monotonic() < end:
            time.sleep(0.01)
        assert got == items, (header, got, "".join(self.log))

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

    def modern(self, method, params=None, rpc_id=1, headers=None, meta=None, conn=None):
        params = dict(params or {})
        params["_meta"] = meta if meta is not None else {
            "io.modelcontextprotocol/protocolVersion": MODERN,
            "io.modelcontextprotocol/clientCapabilities": {},
        }
        all_headers = {"MCP-Protocol-Version": MODERN, "Mcp-Method": method}
        if "name" in params or "taskId" in params:
            all_headers["Mcp-Name"] = params.get("name", params.get("taskId"))
        all_headers.update(headers or {})
        return self.rpc(method, params, rpc_id, all_headers, conn)

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


# The object of the configuration file that each option is in, as
# doc/askai.example.json has it.
with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "doc",
                       "askai.example.json")) as _f:
    GROUPS = {name: group for group, members in json.load(_f).items()
              if group in ("upstream", "service", "testing", "debugging")
              for name in members}


def grouped(members):
    """The configuration file's object for @p members, the options and the
    other members in one object: each option goes into its group's
    object."""
    file = {}
    for name, value in members.items():
        if name in GROUPS:
            file.setdefault(GROUPS[name], {})[name] = value
        else:
            file[name] = value
    return file


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
        "interactions": ("/v1beta/interactions", "x-goog-api-key", "secret", {
            "model": "m", "input": [{"type": "user_input", "content": [
                {"type": "text", "text": "Q"}]}],
            "system_instruction": "SYS", "store": False,
            "generation_config": {"max_output_tokens": 77, "temperature": 0.25},
            "top_p": 0.5, "stream": True}),
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
            if protocol in ("responses", "interactions"):
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

    # Interactions has generation_config only for -n or -t, and -t alone
    # puts only the temperature in it.
    for args, config in (((), None), (("-t", "0.5"), {"temperature": 0.5})):
        with Askai(binary, "--api", "interactions", "-u", up.url("/v1beta/interactions"),
                   "-m", "m", *args) as ai:
            up.reply = lambda request: (200, ANSWERS["interactions"])
            assert text_of(ai.ask()) == "A"
            body = up.last()["body"]
            assert body.get("generation_config") == config, body
            assert not {"temperature", "max_output_tokens"} & set(body), body

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
    print("openai/responses/anthropic/gemini/interactions requests and answers: "
          "PASS")


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

    # These answers of Gemini's Interactions are errors: an error body, an
    # answer that failed with no text, whole or streamed, an error event
    # without an error member, and a stream that ends with [DONE] and has
    # no interaction.completed, also when its events have the types that end
    # an answer of the Responses API or of Anthropic's Messages.
    error_event = sse_wire([("error", {"event_type": "error"})])
    ends = ("response.completed", "response.incomplete", "message_stop")
    cut = sse_of(ANSWERS["interactions"])[:7]
    with Askai(binary, "--api", "interactions",
               "-u", up.url("/v1beta/interactions")) as ai:
        for reply, message in (
                ((400, {"error": {"code": "invalid_request", "message": "bad tool"}}),
                 "Upstream error (HTTP 400): bad tool"),
                ((200, {"status": "failed", "steps": []}),
                 "The model returned no answer (stop reason: failed)."),
                ((200, Stream(wire=error_event)),
                 "Upstream error (HTTP 200): an error event"),
                ((200, Stream(wire=sse_wire(sse_of(ANSWERS["interactions"])[:3] +
                                            [("done", "[DONE]")]))),
                 "Upstream request failed: Transferred a partial file: the stream "
                 "ended before the answer did"),
                ((200, Stream(wire=sse_wire(
                    [(n, d | {"type": ends[i % 3]}) for i, (n, d) in enumerate(cut)] +
                    [("done", "[DONE]")]))),
                 "Upstream request failed: Transferred a partial file: the stream "
                 "ended before the answer did")):
            for plain in (True, False):
                up.plain = plain
                up.reply = lambda request, r=reply: r
                result = ai.ask()
                assert result["isError"] is True and text_of(result) == message, \
                    (plain, text_of(result), message)
        up.plain = False
    up.reply = lambda request: (200, ANSWERS["openai"])

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
        # The list depends on the caller's role, so no cache may share it.
        assert body["result"]["cacheScope"] == "private", body

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
    with tasks/get until the result is in, or ends it with tasks/cancel.
    Only the owner of a task can read it: the client's identity, or the
    address of an anonymous client.  A call that answers sooner, and one of
    a client that does not take tasks, answer as ever."""
    def digest(token):
        return "sha256:" + hashlib.sha256(token.encode()).hexdigest()

    alice, bob = ({"Authorization": f"Bearer {t}"} for t in ("a-1", "b-1"))
    iso = r"\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3}Z"
    hold = up.reply
    with tempfile.TemporaryDirectory() as d:
        path = f"{d}/askai.json"
        with open(path, "w") as f:
            json.dump(grouped({"url": up.url(),
                               "roles": {"asker": {"tools": ["ask_ai"], "metrics": True}},
                               "identities": {"alice": {"role": "asker", "tokens": [digest("a-1")]},
                                              "bob": {"role": "asker", "tokens": [digest("b-1")]}},
                               "anonymous": {"loopback": "asker"}}),
                      f)
        os.chmod(path, 0o600)
        try:
            with Askai(binary, "-c", path, "--verbose") as ai:
                def call(delay, meta=TAKES_TASKS, answer=(200, ANSWERS["openai"])):
                    up.reply = lambda r: (*answer, delay)
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
                direct = result

                # A slow one is a task after a second, whose record its client
                # polls until the call's result is in.
                task, took = call(2)
                assert 0.9 <= took < 1.8, took
                tid = task.pop("taskId")
                ai.wait_log(rf"^askai: tool call ask_ai from identity alice became task "
                            rf"{tid} after \d+\.\d s$")
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
                assert done["status"] == "completed" and done["result"] == direct, \
                    (done, direct)
                assert done["ttlMs"] >= 3_600_000 and done["createdAt"] == task["createdAt"] \
                    and done["createdAt"] < done["lastUpdatedAt"], done

                # The task's result is the result of a direct call also when it
                # is an error of the tool.
                refused = (401, {"error": {"message": "bad key", "type": "auth"}})
                failed, _ = call(0, answer=refused)
                assert failed["isError"] is True and \
                    text_of(failed) == "Upstream error (HTTP 401): bad key", failed
                slow, _ = call(2, answer=refused)
                assert slow["resultType"] == "task", slow
                ended = poll(slow["taskId"])
                assert ended["status"] == "completed" and ended["result"] == failed, \
                    (ended, failed)

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
                ai.wait_log(r"^askai: tool call ask_ai from identity alice cancelled by "
                            rf"its client after \d+\.\d s \(0 bytes\); task "
                            rf"{task['taskId']}; ask call ")
                gone = poll(task["taskId"])
                assert gone["status"] == "cancelled" and "result" not in gone and \
                    gone["statusMessage"], gone

                values = ai.metrics(alice)
                assert values["askai_mcp_tasks_opened_total"] == 3 and \
                    values['askai_mcp_tasks_ended_total{status="completed"}'] == 2 and \
                    values['askai_mcp_tasks_ended_total{status="cancelled"}'] == 1 and \
                    values["askai_mcp_tasks_kept"] == 3 and \
                    values["askai_mcp_task_result_bytes"] > 0, values
                assert total(values, "askai_request_seconds_count", method="tasks/get") >= 4

                # The task of an anonymous client belongs to the client's
                # address.  A client at another address cannot read it, and an
                # identity at the same address cannot either.  The anonymous
                # client cannot read alice's task.  A new connection from the
                # same address reads the task until its result is in.
                def from_address(source, method, task_id, headers=None):
                    conn = http.client.HTTPConnection(ai.host, ai.port, timeout=10,
                                                      source_address=(source, 0))
                    try:
                        params = {"taskId": task_id} if task_id else {
                            "name": "ask_ai", "arguments": {"prompt": "Q"}}
                        return ai.modern(method, params, meta=TAKES_TASKS, headers=headers,
                                         conn=conn)
                    finally:
                        conn.close()

                up.reply = lambda r: (200, ANSWERS["openai"], 1.5)
                status, _, body = from_address("127.0.0.1", "tools/call", None)
                assert status == 200 and body["result"]["resultType"] == "task", body
                anon = body["result"]["taskId"]
                for source, headers, task_id in (("127.0.0.2", None, anon),
                                                 ("127.0.0.1", alice, anon),
                                                 ("127.0.0.1", None, tid)):
                    status, _, body = from_address(source, "tasks/get", task_id, headers)
                    assert status == 200 and "result" not in body and \
                        body["error"]["code"] == -32602, (source, body)
                for _ in range(100):
                    status, _, body = from_address("127.0.0.1", "tasks/get", anon)
                    if body["result"]["status"] != "working":
                        break
                    time.sleep(0.1)
                assert status == 200 and body["result"]["status"] == "completed" and \
                    text_of(body["result"]["result"]) == "A", body
        finally:
            up.reply = hold
    print("MCP tasks: calls that run a second become tasks, polled, bound to their "
          "identity or address, cancelled: PASS")


def check_client_jobs(binary, up):
    """With --client-jobs, askai offers clients background, jobs, wait and
    timer, as their role grants them.  background runs a call of another of
    the client's tools as a job: it answers with the job's id at once, and
    the job runs without the request and its connection.  jobs lists the
    owner's jobs, tasks and timers, answers with a job's result as a direct
    call answers, and cancels a job or a timer.  wait ends when a job ends,
    a timer fires or its seconds pass, and timer sets a timer.  The owner is
    the client's identity, or the address of an anonymous client, on any
    connection and any loop.  A job is a task of MCP's tasks extension too.
    Without the option, askai offers none of the four."""
    def digest(token):
        return "sha256:" + hashlib.sha256(token.encode()).hexdigest()

    alice, bob = ({"Authorization": f"Bearer {t}"} for t in ("a-1", "b-1"))
    started = re.compile(r"Started job ([0-9a-f]{32})\. Call wait to wait for it, "
                         r"then jobs with the action result and this id\.")
    other = "The tool argument must name a tool of yours other than background, " \
        "jobs, wait and timer."

    def done(jid):
        return f"[askai: job {jid}, ask_ai, completed: call jobs with the action " \
            f"result and job {jid} for its result.]\n"

    def send(ai, method, params, source="127.0.0.1", meta=None, headers=None):
        """A request from the address @p source; with @p meta, a modern one
        with that _meta, or with the usual one if it is empty."""
        conn = http.client.HTTPConnection(ai.host, ai.port, timeout=30,
                                          source_address=(source, 0))
        try:
            if meta is not None:
                return ai.modern(method, params, meta=meta or None, headers=headers,
                                 conn=conn)
            return ai.rpc(method, params, headers=headers, conn=conn)
        finally:
            conn.close()

    def call(ai, name, source="127.0.0.1", meta=None, headers=None, **args):
        """The result of a call of @p name from @p source, and its seconds."""
        start = time.monotonic()
        status, _, body = send(ai, "tools/call", {"name": name, "arguments": args},
                               source, meta, headers)
        assert status == 200 and "result" in body, body
        return body["result"], time.monotonic() - start

    def say(ai, name, source="127.0.0.1", **args):
        return text_of(call(ai, name, source, **args)[0])

    def refused(ai, name, message, source="127.0.0.1", **args):
        result, _ = call(ai, name, source, **args)
        assert result.get("isError") is True and text_of(result) == message, \
            (name, args, result)

    def start(ai, source="127.0.0.1", headers=None, prompt="Q"):
        """Start an ask call as a job, which answers at once with its id."""
        result, took = call(ai, "background", source, headers=headers, tool="ask_ai",
                            arguments={"prompt": prompt})
        match = started.fullmatch(text_of(result))
        assert match and "isError" not in result and took < 0.5, (result, took)
        return match.group(1)

    def waits(ai):
        return ai.metrics()["askai_mcp_waits"]

    hold = up.reply
    try:
        with tempfile.TemporaryDirectory() as d:
            path = f"{d}/askai.json"

            def write(members):
                with open(path, "w") as f:
                    json.dump(grouped({"url": up.url()} | members), f)
                os.chmod(path, 0o600)

            # Without the option, askai offers none of the four.  With it, a
            # role may name them, but never compact, and its "upstream" may
            # name none of them.
            with Askai(binary, "-u", up.url()) as ai:
                tools = ai.rpc("tools/list")[2]["result"]["tools"]
                assert [t["name"] for t in tools] == ["ask_ai"], tools
                body = ai.rpc("tools/call", {"name": "background", "arguments": {}})[2]
                assert body["error"]["message"] == "Unknown tool: background", body
            for role, args, message in (
                    ({"tools": ["wait"]}, (), 'role r: askai offers no tool "wait"'),
                    ({"tools": ["compact"]}, ("--client-jobs", "--upstream-tools"),
                     'role r: askai offers no tool "compact"'),
                    ({"upstream": ["timer"]}, ("--client-jobs", "--upstream-tools"),
                     'role r: askai offers no tool "timer" to the upstream')):
                write({"roles": {"r": role}})
                run = subprocess.run([binary, "-l", "127.0.0.1:0", "-c", path, *args],
                                     capture_output=True, text=True, timeout=10)
                assert run.returncode == 2 and message in run.stderr, (role, run.stderr)

            # A role that grants the four, and identities: only the owner of
            # a job can read it, from any address, also after a reload.  A
            # role that grants background but not ask_ai cannot start an
            # ask call as a job.
            write({"roles": {"r": {"tools": ["ask_ai", "background", "jobs", "wait",
                                             "timer"]},
                             "s": {"tools": ["background", "jobs", "wait", "timer"]}},
                   "identities": {"alice": {"role": "r", "tokens": [digest("a-1")]},
                                  "bob": {"role": "r", "tokens": [digest("b-1")]},
                                  "carol": {"role": "s", "tokens": [digest("c-1")]}},
                   "anonymous": {"loopback": "r"}})
            with Askai(binary, "-c", path, "--client-jobs") as ai:
                tools = ai.rpc("tools/list", headers=bob)[2]["result"]["tools"]
                assert [t["name"] for t in tools] == \
                    ["ask_ai", "background", "jobs", "wait", "timer"], tools
                up.reply = lambda r: (200, ANSWERS["openai"], 0.5)
                count = len(up.requests)
                refused(ai, "background", other, headers={"Authorization": "Bearer c-1"},
                        tool="ask_ai", arguments={"prompt": "Q"})
                jid = start(ai, headers=alice)
                assert say(ai, "jobs", headers=bob, action="list") == \
                    "You have no jobs and no timers.", ai.log
                refused(ai, "jobs", "No job of yours has that id; askai stores a job "
                        "for an hour after it ends.", headers=bob, action="result",
                        job=jid)
                assert say(ai, "wait", "127.0.0.2", headers=alice, seconds=10,
                           jobs=[jid]) == done(jid)
                assert len(up.requests) == count + 1, up.requests[count:]
                ai.log.clear()
                ai.proc.send_signal(signal.SIGHUP)
                ai.wait_log("configuration reloaded: policy")
                assert say(ai, "jobs", "127.0.0.3", headers=alice, action="result",
                           job=jid) == "A"
                assert re.fullmatch(f"job {jid}: ask_ai: completed [0-9] s ago\n",
                                    say(ai, "jobs", headers=alice, action="list"))

            # A job that one loop runs ends a wait on another: the job's
            # connection stays open, so the next connections go to the other
            # loop first.  Each of the three waits names the job, and so
            # each of them gets it, as the job ends and not at the end of
            # the wait's seconds.
            with Askai(binary, "-u", up.url(), "--client-jobs", loops=2) as ai:
                up.reply = lambda r: (200, ANSWERS["openai"], 1.5)
                conn = ai.connect()
                status, _, body = ai.rpc("tools/call", {"name": "background", "arguments": {
                    "tool": "ask_ai", "arguments": {"prompt": "Q"}}}, conn=conn)
                assert status == 200, body
                jid = started.fullmatch(text_of(body["result"])).group(1)
                said = []
                threads = [threading.Thread(target=lambda: said.append(
                    call(ai, "wait", seconds=10, jobs=[jid]))) for _ in range(3)]
                for thread in threads:
                    thread.start()
                for thread in threads:
                    thread.join()
                conn.close()
                assert [text_of(r) for r, _ in said] == [done(jid)] * 3 and \
                    all(took < 3 for _, took in said), said

            # With --upstream-tools too, the turns get the four with their own
            # definitions, compact last.  The timers of the turns of a
            # client's ask job are not timers of the client.
            repo = f"{d}/proj"
            subprocess.run(["git", "init", "-q", repo], check=True)
            script = []
            up.reply = lambda r: script.pop(0)(r) if script else \
                (200, turn_stream(text="A"))
            with Askai(binary, "-u", up.url(), "--client-jobs", "--upstream-tools",
                       "--git", repo) as ai:
                tools = ai.rpc("tools/list")[2]["result"]["tools"]
                assert [t["name"] for t in tools] == \
                    ["ask_ai", "git", "background", "jobs", "wait", "timer"], tools
                script[:] = [
                    lambda r: (200, turn_stream([("t1", "timer", json.dumps(
                        {"seconds": 1, "note": "turn"}))])),
                    lambda r: (200, turn_stream(text="Waiting.")),
                    lambda r: (200, turn_stream(text="Done."))]
                count = len(up.requests)
                jid = start(ai)
                wait_for(lambda: len(up.requests) >= count + 2)
                lent = up.requests[count]["body"]["tools"]
                assert [t["function"]["name"] for t in lent] == \
                    ["git", "background", "jobs", "wait", "timer", "compact"], lent
                assert lent[2]["function"]["parameters"]["properties"]["job"]["type"] == \
                    "integer", lent[2]
                assert re.fullmatch(f"job {jid}: ask_ai: running for [0-9] s\n",
                                    say(ai, "jobs", action="list"))
                assert say(ai, "wait", seconds=10, jobs=[jid]) == done(jid)
                result, _ = call(ai, "jobs", action="result", job=jid)
                assert result["content"][0]["text"] == \
                    "Waiting.\n\n[askai: timer 1 fired: turn.]\n\nDone.", result
                assert re.fullmatch(f"job {jid}: ask_ai: completed [0-9] s ago\n",
                                    say(ai, "jobs", action="list"))
            up.reply = hold

            with Askai(binary, "-u", up.url(), "--client-jobs", "--slots", "16",
                       "--verbose") as ai:
                tools = {t["name"]: t for t in ai.rpc("tools/list")[2]["result"]["tools"]}
                assert list(tools) == ["ask_ai", "background", "jobs", "wait", "timer"], \
                    tools
                assert "such as ask_ai," in tools["background"]["description"], tools
                props = {name: t["inputSchema"]["properties"] for name, t in tools.items()}
                assert props["wait"]["jobs"]["items"] == {"type": "string"} and \
                    props["jobs"]["job"]["type"] == "string" and \
                    props["jobs"]["timer"]["type"] == "integer", props

                # A background ask answers at once, and runs as a job, which
                # the list shows and a wait waits for.  The job's result is
                # the result of a direct call, in either era.  The wait
                # reported the job, so the list shows it as read.  Each call
                # here has a connection of its own, which closes after it.
                up.reply = lambda r: (200, ANSWERS["openai"], 2)
                jid = start(ai, prompt="SECRET")
                assert re.fullmatch(f"job {jid}: ask_ai: running for [01] s\n",
                                    say(ai, "jobs", action="list"))
                refused(ai, "jobs", "That job still runs: wait for it.", action="result",
                        job=jid)
                result, took = call(ai, "wait", seconds=10, jobs=[jid])
                assert text_of(result) == done(jid) and 1.2 <= took < 3, (result, took)
                assert re.fullmatch(f"job {jid}: ask_ai: completed [0-9] s ago\n",
                                    say(ai, "jobs", action="list"))
                up.reply = lambda r: (200, ANSWERS["openai"])
                legacy, _ = call(ai, "jobs", action="result", job=jid)
                assert legacy == ai.ask() and "resultType" not in legacy, legacy
                modern, _ = call(ai, "jobs", meta={}, action="result", job=jid)
                direct = ai.modern("tools/call", {"name": "ask_ai",
                                                  "arguments": {"prompt": "Q"}})[2]
                assert modern == direct["result"] and modern["resultType"] == "complete", \
                    (modern, direct)

                # What background, jobs and wait refuse.
                for args in ({"tool": "wait", "arguments": {"seconds": 1}},
                             {"tool": "background"}, {"tool": "nope"}, {}):
                    refused(ai, "background", other, **args)
                refused(ai, "background", "The arguments argument must be an object.",
                        tool="ask_ai", arguments=[])
                refused(ai, "jobs", "The action argument must be list, result or cancel.",
                        action="show")
                for job in ("0" * 31, 7, None):
                    refused(ai, "jobs", "The job argument must be the id of a job of "
                            "yours.", action="result", job=job)
                refused(ai, "jobs", "No job of yours has that id; askai stores a job for "
                        "an hour after it ends.", action="cancel", job="0" * 32)
                for args in ({}, {"job": jid, "timer": 1}):
                    refused(ai, "jobs", "The job argument or the timer argument must "
                            "name what to cancel.", action="cancel", **args)
                refused(ai, "jobs", "That job runs no more.", action="cancel", job=jid)
                for seconds in (0, 601, "1"):
                    refused(ai, "wait", "The seconds argument must be a whole number "
                            "from 1 to 600.", seconds=seconds)
                for jobs in ("x", [1], ["0" * 32] * 17):
                    refused(ai, "wait", "The jobs argument must list at most 16 ids of "
                            "jobs of yours.", seconds=1, jobs=jobs)
                assert say(ai, "wait", seconds=1, jobs=[jid] * 16) == done(jid)
                refused(ai, "wait", 'No job of yours has the id \u00e9\\"' + "x" * 28 +
                        "; askai stores a job for an hour after it ends.", seconds=1,
                        jobs=[jid, '\u00e9\\"' + "x" * 40])

                # An owner runs 8 jobs and tasks at most.  A wait for any job
                # ends as soon as a job ends that no one has read.
                up.reply = lambda r: (200, ANSWERS["openai"], 1.5)
                ids = [start(ai, "127.0.0.2") for _ in range(8)]
                refused(ai, "background", "You have 8 jobs and tasks running, the most "
                        "at once: wait for one to end.", "127.0.0.2", tool="ask_ai",
                        arguments={"prompt": "Q"})
                seen = []
                for _ in range(8):
                    if len(seen) == 8:
                        break
                    text = say(ai, "wait", "127.0.0.2", seconds=10)
                    seen += re.findall(r"^\[askai: job ([0-9a-f]{32}), ask_ai, completed: "
                                       r"call jobs with the action result and job \1 for "
                                       r"its result\.\]$", text, re.M)
                assert sorted(seen) == sorted(ids), (seen, ids)
                assert say(ai, "jobs", "127.0.0.2", action="list").count(" ago\n") == 8

                # Another address is another owner, also for tasks/get.
                refused(ai, "jobs", "No job of yours has that id; askai stores a job for "
                        "an hour after it ends.", action="result", job=ids[0])
                for source, found in (("127.0.0.1", False), ("127.0.0.2", True)):
                    body = send(ai, "tasks/get", {"taskId": ids[0]}, source, TAKES_TASKS)[2]
                    assert ("result" in body) == found and \
                        (not found or body["result"]["status"] == "completed"), body

                # A cancel of a job that runs cancels its upstream call.  The
                # upstream call is in the background class.
                up.cancelled.clear()
                up.reply = lambda r: (200, ANSWERS["openai"], 30)
                count = len(up.requests)
                jid = start(ai, "127.0.0.3")
                wait_for(lambda: len(up.requests) == count + 1)
                assert ai.metrics()["askai_upstream_background_calls"] == 1
                assert say(ai, "jobs", "127.0.0.3", action="cancel", job=jid) == \
                    f"askai is cancelling job {jid}. Call wait to wait for its end."
                assert up.cancelled.wait(5)
                assert say(ai, "wait", "127.0.0.3", seconds=5, jobs=[jid]) == \
                    f"[askai: job {jid}, ask_ai, cancelled.]\n"
                refused(ai, "jobs", "That job was cancelled.", "127.0.0.3", action="result",
                        job=jid)
                assert re.fullmatch(f"job {jid}: ask_ai: cancelled [0-9] s ago\n",
                                    say(ai, "jobs", "127.0.0.3", action="list"))
                time.sleep(0.3)
                assert len(up.requests) == count + 1, up.requests[count:]

                # Timers: a wait ends as one fires, with its note, or after its
                # seconds; a timer that has not fired can be cancelled; an
                # owner has 8 timers at most.
                said = say(ai, "timer", "127.0.0.4", seconds=1, note="n")
                n = re.fullmatch(r"Timer (\d+) will fire in 1 s\.", said).group(1)
                assert say(ai, "jobs", "127.0.0.4", action="list") == \
                    f"timer {n}: fires in 1 s: n\n"
                result, took = call(ai, "wait", "127.0.0.4", seconds=5)
                assert text_of(result) == f"[askai: timer {n} fired: n.]\n" and \
                    0.7 <= took < 2, (result, took)
                result, took = call(ai, "wait", "127.0.0.4", seconds=1)
                assert text_of(result) == "Nothing happened in 1 s." and \
                    0.9 <= took < 2, (result, took)
                # A wait that waits answers on an event stream, which the
                # client accepts.
                _, headers, body = send(ai, "tools/call", {"name": "wait", "arguments": {
                    "seconds": 1}}, "127.0.0.4")
                assert headers["content-type"] == "text/event-stream" and \
                    text_of(body["result"]) == "Nothing happened in 1 s.", (headers, body)
                n = re.fullmatch(r"Timer (\d+) will fire in 60 s\.",
                                 say(ai, "timer", "127.0.0.4", seconds=60)).group(1)
                assert say(ai, "jobs", "127.0.0.4", action="cancel", timer=int(n)) == \
                    f"Cancelled timer {n}."
                refused(ai, "jobs", f"You have no timer {n} set.", "127.0.0.4",
                        action="cancel", timer=int(n))
                for timer in (n, float(n), True):
                    refused(ai, "jobs", "The timer argument must be the number of a timer "
                            "of yours.", "127.0.0.4", action="cancel", timer=timer)
                for _ in range(8):
                    assert say(ai, "timer", "127.0.0.4", seconds=60).startswith("Timer ")
                refused(ai, "timer", "You have 8 timers set, the most at once.",
                        "127.0.0.4", seconds=60)
                for args, message in (
                        ({"seconds": 0}, "The seconds argument must be a whole number "
                                         "from 1 to 3600."),
                        ({"seconds": 3601}, "The seconds argument must be a whole number "
                                            "from 1 to 3600."),
                        ({"seconds": 1, "note": "x" * 201},
                         "The note argument must be a string of at most 200 bytes.")):
                    refused(ai, "timer", message, "127.0.0.5", **args)
                assert say(ai, "jobs", "127.0.0.4", action="list").count(
                    ": fires in 60 s\n") == 8

                # Two waits for any job of one owner: one event ends one.
                said = []
                threads = [threading.Thread(target=lambda: said.append(
                    say(ai, "wait", "127.0.0.6", seconds=3))) for _ in range(2)]
                for thread in threads:
                    thread.start()
                wait_for(lambda: waits(ai) == 2)
                up.reply = lambda r: (200, ANSWERS["openai"], 0.3)
                jid = start(ai, "127.0.0.6")
                for thread in threads:
                    thread.join()
                assert sorted(said) == sorted([done(jid), "Nothing happened in 3 s."]), said

                # An owner has 4 waits at once at most.
                socks = []
                for i in range(4):
                    socks.append(socket.create_connection(("127.0.0.1", ai.port), 10,
                                                          ("127.0.0.7", 0)))
                    socks[-1].sendall(rpc_request(i, {"name": "wait", "arguments": {
                        "seconds": 30}}, "tools/call"))
                wait_for(lambda: waits(ai) == 4)
                refused(ai, "wait", "You have 4 waits under way, the most at once.",
                        "127.0.0.7", seconds=1)
                for sock in socks:
                    sock.close()
                wait_for(lambda: waits(ai) == 0)

                # The tasks extension: tasks/get of a job polls it, and its
                # result is the job's; tasks/cancel cancels a job; a call that
                # became a task is in the list; a wait of a client that takes
                # tasks never becomes a task.
                up.reply = lambda r: (200, ANSWERS["openai"], 1.5)
                jid = start(ai, "127.0.0.8")
                for _ in range(100):
                    task = send(ai, "tasks/get", {"taskId": jid}, "127.0.0.8",
                                TAKES_TASKS)[2]["result"]
                    if task["status"] != "working":
                        break
                    time.sleep(0.1)
                assert re.fullmatch(f"job {jid}: ask_ai: completed [0-9] s ago\n",
                                    say(ai, "jobs", "127.0.0.8", action="list"))
                assert task["status"] == "completed" and task["result"] == \
                    call(ai, "jobs", "127.0.0.8", meta={}, action="result", job=jid)[0], task
                up.cancelled.clear()
                up.reply = lambda r: (200, ANSWERS["openai"], 30)
                count = len(up.requests)
                jid = start(ai, "127.0.0.8")
                wait_for(lambda: len(up.requests) == count + 1)
                assert send(ai, "tasks/cancel", {"taskId": jid}, "127.0.0.8",
                            TAKES_TASKS)[0] == 200
                assert up.cancelled.wait(5)
                assert say(ai, "wait", "127.0.0.8", seconds=5, jobs=[jid]) == \
                    f"[askai: job {jid}, ask_ai, cancelled.]\n"
                up.reply = lambda r: (200, ANSWERS["openai"], 2)
                task, _ = call(ai, "ask_ai", "127.0.0.8", meta=TAKES_TASKS, prompt="Q")
                assert task["resultType"] == "task", task
                assert re.search(f"^task {task['taskId']}: ask_ai: running for [0-9] s$",
                                 say(ai, "jobs", "127.0.0.8", action="list"), re.M)
                result, took = call(ai, "wait", "127.0.0.9", meta=TAKES_TASKS, seconds=2)
                assert result["resultType"] == "complete" and \
                    text_of(result) == "Nothing happened in 2 s." and 1.8 <= took < 3, \
                    (result, took)
                assert say(ai, "wait", "127.0.0.8", seconds=5, jobs=[task["taskId"]]) == \
                    f"[askai: task {task['taskId']}, ask_ai, completed: call jobs with " \
                    f"the action result and job {task['taskId']} for its result.]\n"

                # The metrics count the jobs and the refusals, the timers set,
                # and the waits that wait now.  Only two lines of --verbose
                # have the prompt: the start of the ask call and the end of
                # the job.
                values = ai.metrics()
                assert values["askai_mcp_jobs_opened_total"] == 13 and \
                    values["askai_mcp_jobs_refused_total"] == 1 and \
                    values["askai_mcp_tasks_opened_total"] == 1 and \
                    values["askai_mcp_timers_set_total"] == 10 and \
                    values["askai_mcp_waits"] == 0, values
                assert [re.sub(r"\d+\.\d+", "N", re.sub(r"[0-9a-f]{32}", "ID", line))
                        for line in ai.log if "SECRET" in line] == [
                    'askai: ask call N from anonymous: prompt 6 bytes, no thread, '
                    '"SECRET"\n',
                    "askai: tool call ask_ai from anonymous answered after N s (1 byte); "
                    "job ID; ask call N, 1 turn, 0 tool calls; arguments "
                    '{"prompt":"SECRET"}\n'], ai.log

                # A job that has ended is unread in the list until jobs
                # returns its result.  A wait for any job then does not
                # report it.
                src = "127.0.1.1"
                up.reply = lambda r: (200, ANSWERS["openai"], 0.3)
                jid = start(ai, src)
                for _ in range(50):
                    listed = say(ai, "jobs", src, action="list")
                    if "running" not in listed:
                        break
                    time.sleep(0.1)
                assert re.fullmatch(f"job {jid}: ask_ai: completed [0-9] s ago, unread\n",
                                    listed), listed
                assert say(ai, "jobs", src, action="result", job=jid) == "A"
                assert re.fullmatch(f"job {jid}: ask_ai: completed [0-9] s ago\n",
                                    say(ai, "jobs", src, action="list"))
                assert say(ai, "wait", src, seconds=1) == "Nothing happened in 1 s."

                # A wait that names a job that runs ends when a timer of the
                # owner fires.
                src = "127.0.1.7"
                up.reply = lambda r: (200, ANSWERS["openai"], 5)
                jid = start(ai, src)
                n = re.fullmatch(r"Timer (\d+) will fire in 1 s\.",
                                 say(ai, "timer", src, seconds=1, note="t")).group(1)
                result, took = call(ai, "wait", src, seconds=10, jobs=[jid])
                assert text_of(result) == f"[askai: timer {n} fired: t.]\n" and took < 2.5, \
                    (result, took)
                say(ai, "jobs", src, action="cancel", job=jid)
                assert say(ai, "wait", src, seconds=5, jobs=[jid]) == \
                    f"[askai: job {jid}, ask_ai, cancelled.]\n"

                # The list has the newest job first, and the timers by
                # number, also after a timer has the place of a cancelled
                # one.
                src = "127.0.1.8"
                up.reply = lambda r: (200, ANSWERS["openai"])
                first = start(ai, src)
                assert say(ai, "wait", src, seconds=5, jobs=[first]) == done(first)
                second = start(ai, src)
                assert say(ai, "wait", src, seconds=5, jobs=[second]) == done(second)

                def timer(src):
                    return int(re.fullmatch(r"Timer (\d+) will fire in 60 s\.",
                                            say(ai, "timer", src, seconds=60)).group(1))
                numbers = [timer(src) for _ in range(3)]
                assert say(ai, "jobs", src, action="cancel", timer=numbers[0]) == \
                    f"Cancelled timer {numbers[0]}."
                numbers.append(timer(src))
                listed = say(ai, "jobs", src, action="list")
                assert re.findall(r"^job ([0-9a-f]{32}):", listed, re.M) == [second, first], \
                    listed
                assert [int(n) for n in re.findall(r"^timer (\d+):", listed, re.M)] == \
                    numbers[1:], listed

                # A timer that has fired is unread in the list until a wait
                # reports it.  It cannot be cancelled, and the answer says
                # so.  A wait then reports the timer at once, and the owner
                # has nothing left.
                src = "127.0.1.6"
                n = re.fullmatch(r"Timer (\d+) will fire in 1 s\.",
                                 say(ai, "timer", src, seconds=1, note="n")).group(1)
                time.sleep(1.3)
                assert re.fullmatch(f"timer {n}: fired [0-9] s ago, unread: n\n",
                                    say(ai, "jobs", src, action="list"))
                refused(ai, "jobs", f"Timer {n} has fired, and a call of wait reports it.",
                        src, action="cancel", timer=int(n))
                result, took = call(ai, "wait", src, seconds=5)
                assert text_of(result) == f"[askai: timer {n} fired: n.]\n" and took < 0.5, \
                    (result, took)
                assert say(ai, "jobs", src, action="list") == "You have no jobs and no timers."

                # A wait ends with an error as askai stops.
                with socket.create_connection(("127.0.0.1", ai.port), 10,
                                              ("127.0.0.10", 0)) as sock:
                    sock.sendall(rpc_request(5, {"name": "wait", "arguments": {
                        "seconds": 30}}, "tools/call"))
                    wait_for(lambda: waits(ai) == 1)
                    ai.proc.send_signal(signal.SIGTERM)
                    status, body = Replies(sock).next()
                    assert status == 503 and \
                        body["error"]["message"] == "Server is shutting down", body
                assert ai.proc.wait(timeout=5) == 0, "".join(ai.log)

            # Anonymous addresses have room for 512 owners, half of the
            # table of owners.  A timer of the next address gets the answer
            # that askai has no room.
            with Askai(binary, "-u", up.url(), "--client-jobs") as ai:
                for k in range(512):
                    assert say(ai, "timer", f"127.0.{2 + k // 256}.{k % 256}",
                               seconds=3600).startswith("Timer "), k
                refused(ai, "timer", "askai has no room for more jobs, timers or waits "
                        "now; try again later.", "127.0.9.1", seconds=1)

            # askai stops at once, with exit status 0, while a job runs: a
            # job that waits for the upstream, and a job whose turn waits in
            # the agenda of its ask call.
            up.reply = lambda r: (200, ANSWERS["openai"], 30)
            with Askai(binary, "-u", up.url(), "--client-jobs") as ai:
                count = len(up.requests)
                start(ai)
                wait_for(lambda: len(up.requests) == count + 1)
            script[:] = [lambda r: (200, turn_stream([("w1", "wait", json.dumps(
                {"seconds": 30}))]))]
            up.reply = lambda r: script.pop(0)(r) if script else \
                (200, turn_stream(text="A"))
            with Askai(binary, "-u", up.url(), "--client-jobs", "--upstream-tools",
                       "--git", repo) as ai:
                count = len(up.requests)
                jid = start(ai)
                wait_for(lambda: len(up.requests) == count + 1)
                assert re.fullmatch(f"job {jid}: ask_ai: running for [0-9] s\n",
                                    say(ai, "jobs", action="list"))
    finally:
        up.reply = hold
    print("client jobs: background, jobs, wait and timer with --client-jobs, by "
          "owner, across loops and as tasks: PASS")


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
    # on the loop that has none, so each notification reaches the call from
    # the other loop, through the roster (src/roster.h).
    with Askai(binary, "-u", up.url(), "--verbose", loops=2) as ai:
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
            ai.log.clear()
            thread.start()
            assert up.arrived.wait(5)
            status, _, _ = ai.rpc("notifications/cancelled",
                                  {"requestId": 42, "reason": "stop"},
                                  rpc_id=None, headers=session)
            assert status == 202
            thread.join(5)
            ai.wait_log(r"^askai: tool call ask_ai from anonymous cancelled by "
                        r"notifications/cancelled after \d+\.\d s \(0 bytes\); ask call "
                        r'\d+\.\d+, 1 turn, 0 tool calls; arguments \{"prompt":"SLOW"\}$')
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
        ai.log.clear()
        with socket.create_connection(("127.0.0.1", ai.port), 10) as sock:
            sock.sendall(b"POST /mcp HTTP/1.1\r\nContent-Type: application/json\r\n"
                         b"Content-Length: %d\r\n\r\n" % len(body) + body)
            assert up.arrived.wait(5)
        assert up.cancelled.wait(2), "client hang-up did not cancel upstream"
        ai.wait_log(r"^askai: tool call ask_ai from anonymous cancelled by a closed "
                    r"connection after ")
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

    # A notification cancels only its caller's own calls.  The caller is the
    # identity, from any address, or for an anonymous client its address.
    # alice may call ask_ai, bob may call no tool, and an anonymous client
    # on loopback may call ask_ai.
    def digest(token):
        return "sha256:" + hashlib.sha256(token.encode()).hexdigest()

    alice = {"Authorization": "Bearer alice"}
    bob = {"Authorization": "Bearer bob"}
    with tempfile.TemporaryDirectory() as d:
        path = f"{os.path.realpath(d)}/askai.json"
        with open(path, "w") as f:
            json.dump(grouped({"url": up.url(),
                               "roles": {"caller": {"tools": ["ask_ai"]}, "none": {"tools": []}},
                               "identities": {"alice": {"role": "caller", "tokens": [digest("alice")]},
                                              "bob": {"role": "none", "tokens": [digest("bob")]}},
                               "anonymous": {"loopback": "caller"}}), f)
        os.chmod(path, 0o600)
        up.reply = slow
        with Askai(binary, "-c", path, loops=2) as ai:
            def connection(source):
                return http.client.HTTPConnection(ai.host, ai.port, timeout=10,
                                                  source_address=(source, 0))

            def attempt(rpc_id, caller, canceller, modern=False):
                """Starts an ask call of caller with rpc_id, and cancels it
                from canceller.  Each is (headers, source address).  Returns
                the call's reply and whether its upstream request was
                cancelled."""
                up.arrived.clear()
                up.cancelled.clear()
                results = []
                params = {"name": "ask_ai", "arguments": {"prompt": "SLOW"}}
                start = (lambda: ai.modern("tools/call", params, rpc_id=rpc_id,
                                           headers=caller[0])) if modern else \
                    (lambda: ai.rpc("tools/call", params, rpc_id=rpc_id,
                                    headers=caller[0], conn=connection(caller[1])))
                thread = threading.Thread(target=lambda: results.append(start()))
                thread.start()
                assert up.arrived.wait(5)
                headers = canceller[0] | ({"MCP-Protocol-Version": MODERN} if modern else {})
                assert ai.rpc("notifications/cancelled", {"requestId": rpc_id},
                              rpc_id=None, headers=headers,
                              conn=connection(canceller[1]))[0] == 202
                thread.join(5)
                return results[0], up.cancelled.wait(0.5)

            def survived(outcome):
                (status, _, body), cancelled = outcome
                return status == 200 and body is not None and \
                    text_of(body["result"]) == "A" and not cancelled

            def ended(outcome):
                """The call ended without a result: an event stream (200)
                without an event, or a JSON reply with 202."""
                (status, _, body), cancelled = outcome
                return status in (200, 202) and body is None and cancelled

            local, other = "127.0.0.1", "127.0.0.2"
            assert survived(attempt(1, (alice, local), (bob, local)))
            assert survived(attempt(2, (alice, local), ({}, local)))
            assert survived(attempt(3, (alice, local), ({}, local), modern=True))
            assert ended(attempt(8, (alice, local), (alice, other), modern=True))
            assert ended(attempt(4, (alice, local), (alice, other)))
            assert survived(attempt(5, ({}, local), ({}, other)))
            assert ended(attempt(6, ({}, local), ({}, local)))

            # A call that has become a task is no longer in the roster.  A
            # notification for its request id leaves the task working, and
            # tasks/cancel ends it.
            up.reply = lambda request: (200, ANSWERS["openai"], 3)
            status, _, body = ai.modern("tools/call", {
                "name": "ask_ai", "arguments": {"prompt": "T"}}, rpc_id=7,
                meta=TAKES_TASKS, headers=alice)
            task = body["result"]["taskId"]
            for headers in (alice, bob, {}):
                assert ai.rpc("notifications/cancelled", {"requestId": 7},
                              rpc_id=None, headers=headers)[0] == 202
            time.sleep(0.3)
            status, _, body = ai.modern("tasks/get", {"taskId": task},
                                        meta=TAKES_TASKS, headers=alice)
            assert status == 200 and body["result"]["status"] == "working", body
            assert ai.modern("tasks/cancel", {"taskId": task}, meta=TAKES_TASKS,
                             headers=alice)[0] == 200
            wait_for(lambda: ai.modern("tasks/get", {"taskId": task}, meta=TAKES_TASKS,
                                       headers=alice)[2]["result"]["status"] == "cancelled")

    # Between the turns of an ask call no job of the call is in home.  A
    # notification then ends the call at once, and the call makes no more
    # upstream requests.  Seventeen calls doze on a timer at the same time.
    turns = {}

    def dozing(request):
        prompt = request["body"]["messages"][0]["content"]
        turns[prompt] = turns.get(prompt, 0) + 1
        if turns[prompt] == 1:
            return 200, turn_stream([("t", "timer", '{"seconds":3}')])
        return 200, turn_stream(text="WAITING" if turns[prompt] == 2 else "AGAIN")

    up.reply = dozing
    sockets = []
    try:
        with Askai(binary, "-u", up.url(), "--man", "--upstream-tools",
                   "--max-client-conns", "64", loops=2) as ai:
            for i in range(17):
                sock = socket.create_connection(("127.0.0.1", ai.port), 10)
                sockets.append(sock)
                sock.sendall(rpc_request(100 + i, {"name": "ask_ai", "arguments": {
                    "prompt": f"call{i}"}}, "tools/call", accept_sse=True))
                wait_for(lambda i=i: turns.get(f"call{i}") == 2)
            wait_for(lambda: total(ai.metrics(), "askai_jobs_in_flight") == 0)
            for i in range(17):
                assert ai.rpc("notifications/cancelled", {"requestId": 100 + i},
                              rpc_id=None)[0] == 202
            # Each event stream ends with its last chunk, and without an
            # event, within a second of the cancels.  The first timer is due
            # 3 s after the first call began, so a call that ends only when
            # its timer fires fails this.
            cancelled = time.monotonic()
            for sock in sockets:
                sock.settimeout(max(0.01, cancelled + 1 - time.monotonic()))
                seen = b""
                while not seen.endswith(b"\r\n0\r\n\r\n"):
                    seen += sock.recv(65536)
                assert b"data:" not in seen, seen
            # No call makes a third upstream request when its timer is due.
            time.sleep(max(0, cancelled + 3.5 - time.monotonic()))
            assert all(n == 2 for n in turns.values()), turns
    finally:
        for sock in sockets:
            sock.close()

    # A notification ends a call that runs without the upstream, here a
    # test_task call of 10,000 slices, at once.
    with Askai(binary, "-u", up.url(), "--test-task", loops=2) as ai:
        session = {"Mcp-Session-Id": "a" * 32}
        results = []
        thread = threading.Thread(target=lambda: results.append(ai.rpc(
            "tools/call", {"name": "test_task", "arguments": {"slices": 10000}},
            rpc_id=99, headers=session)))
        start = time.monotonic()
        thread.start()
        time.sleep(0.2)
        assert ai.rpc("notifications/cancelled", {"requestId": 99}, rpc_id=None,
                      headers=session)[0] == 202
        thread.join(5)
        assert results[0][0] == 202 and results[0][2] is None, results
        assert time.monotonic() - start < 1, "the task ran on after the cancel"
    up.reply = lambda request: (200, ANSWERS["openai"])

    print("cancellation by notification, from another loop too, only of the "
          "caller's own calls, between turns, of a task, hang-up and "
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

    # A reply that is larger than the output pause is sent in full.  With
    # --output-pause 1, half of the pause is 0 bytes, so the stream resumes
    # when no output is pending.
    text = "x" * (600 << 10)
    up.reply = lambda request: (200, {"choices": [{"message": {
        "role": "assistant", "content": text}, "finish_reason": "stop"}]})
    for pause in ("1", "2"):
        with Askai(binary, "-u", up.url(), "--output-pause", pause) as ai:
            assert text_of(ai.ask()) == text, pause
    up.reply = lambda request: (200, ANSWERS["openai"])

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
    for protocol in ("openai", "responses", "anthropic", "gemini", "interactions"):
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
                "interactions": lambda b: (b["input"][0]["content"][0]["text"],
                                           b["system_instruction"]),
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


def check_configuration(binary, up, godbolt):
    def fails(*args, env=None, message):
        child_env = {k: v for k, v in os.environ.items() if k not in KEY_VARS}
        child_env.update(env or {})
        run = subprocess.run([binary, "-l", "127.0.0.1:0", *args], env=child_env,
                             stdin=subprocess.DEVNULL, capture_output=True,
                             text=True, timeout=5, check=False)
        assert run.returncode == 2 and message in run.stderr, (args, run.stderr)

    # The usage block of README.md is the option part of --help: from the
    # --config entry to the blank line before the text about who may connect.
    usage = subprocess.run([binary, "--help"], capture_output=True, text=True,
                           timeout=5, check=True).stdout.split("\n")
    first = next(i for i, line in enumerate(usage) if line.startswith("  -c, --config"))
    last = usage.index("The members roles, identities, unix-groups and anonymous of the") - 1
    with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..",
                           "README.md")) as f:
        readme = f.read().split("\n")
    upstream = readme.index('Upstream AI ("upstream"):')
    start = max(i for i in range(upstream) if readme[i].startswith("```")) + 1
    end = readme.index("```", upstream)
    assert readme[start:end] == usage[first:last], \
        "the usage block of README.md differs from --help"

    # doc/configuration.md documents under a heading "#### `NAME`" each long
    # option of --help but config and help, and each member of the file that
    # is not an option, and nothing else.  Its JSON examples are strict JSON.
    doc = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "doc")
    with open(os.path.join(doc, "configuration.md")) as f:
        reference = f.read()
    def constant(name):
        raise ValueError(name)
    for block in re.findall(r"^```json\n(.*?)^```$", reference, re.M | re.S):
        json.loads(block, parse_constant=constant)
    options = subprocess.run([binary, "--help"], capture_output=True, text=True,
                             timeout=5, check=True).stdout
    options = set(re.findall(r"--([a-z-]+)", options)) - {"config", "help"}
    policy = {"roles", "identities", "unix-groups", "anonymous"}
    others = policy | {"plugins", "tools"}
    documented = re.findall(r"^#### `([a-z-]+)`$", reference, re.M)
    assert len(documented) == len(set(documented)), documented
    assert set(documented) == options | others, set(documented) ^ (options | others)

    # doc/askai.example.json is strict JSON in the format of jq.  Its objects
    # upstream, service, testing and debugging have every option, and it has
    # the other members too.  askai starts with it, once the paths in it are
    # those of files that exist, and its key variable is set; the listen
    # addresses and the URL come from the command line.  The godbolt plugin
    # is the one built beside askai.
    with open(os.path.join(doc, "askai.example.json")) as f:
        example = f.read()
    members = json.loads(example, parse_constant=constant)
    assert example == json.dumps(members, indent=2) + "\n", "not in the format of jq"
    groups = ("upstream", "service", "testing", "debugging")
    assert set(members) == set(groups) | others, set(members) ^ (set(groups) | others)
    assert set(GROUPS) == options, set(GROUPS) ^ options
    with tempfile.TemporaryDirectory() as d:
        d = os.path.realpath(d)
        subprocess.run(["git", "init", "-q", f"{d}/proj"], check=True)
        members["upstream"]["notes"] = f"{d}/notes"
        members["service"]["git"] = [f"{d}/proj"]
        members["plugins"][0]["path"] = os.path.realpath(godbolt)
        with open(f"{d}/askai.json", "w") as f:
            json.dump(members, f)
        os.chmod(f"{d}/askai.json", 0o600)
        with Askai(binary, "-c", f"{d}/askai.json", "-u", up.url(),
                   env={members["upstream"]["key-env"]: "k"}) as ai:
            tools = ai.rpc("tools/list")[2]["result"]["tools"]
            assert [t["name"] for t in tools] == ["ask_ai", "man", "notes"], tools
            assert ai.get("/metrics")[0] == 403
            # The member tools denies godbolt_compile to the upstream, which
            # the role user does not list.
            assert [item.split()[-1] for item in ai.listed("role user:")] == \
                ["ask_ai", "man", "notes", "man", "git", "notes", "background", "jobs",
                 "wait", "timer", "godbolt_catalog", "godbolt_targets", "compact",
                 "no"], ai.log

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
    fails("-p", "llama", "--api", "interactions",
          message="llama has no interactions API")
    fails("-p", "openai", "--api", "interactions", "-m", "m",
          env={"OPENAI_API_KEY": "k"}, message="openai has no interactions API")
    fails("-p", "google", "--api", "interactions", env={"GEMINI_API_KEY": "k"},
          message="--model is required")
    fails("-u", "ftp://x", message="not an http(s) URL")
    fails("-t", "warm", message="invalid temperature")
    fails("-t", "-1", message="invalid temperature")
    # json-c parses NaN and Infinity, and it parses 1e400 as an infinite
    # double.  JSON has no number that is NaN or infinite.
    for temperature in ("NaN", "Infinity", "1e400"):
        fails("-t", temperature, message="invalid temperature: " + temperature)
    fails("-n", "0", message="invalid max token count")
    fails("-x", "[1]", message="needs a JSON object")
    for extra in ('{"top_p": Infinity}', '{"a": [1, {"b": NaN}]}',
                  '{"a": -1e400}'):
        fails("-x", extra, message="--extra contains NaN, an infinity or a "
              "number outside the range of a double: " + extra)
    fails("-H", "Bad Header: x", message="invalid header line")
    fails("--tool-name", "ask ai", message="tool names")
    fails("-u", up.url(), "--upstream-tools", "--tool-name", "wait",
          message="--tool-name: askai has another tool named wait")
    fails("-u", up.url(), "--client-jobs", "--tool-name", "timer",
          message="--tool-name: askai has another tool named timer")
    fails("--max-body", "1X", message="invalid --max-body")
    fails("--max-head", "65536", message="invalid --max-head")
    fails("--max-depth", "0", message="invalid --max-depth")
    fails("--sse-keepalive", "x", message="invalid --sse-keepalive")
    fails("--max-members", "0", message="invalid --max-members")
    fails("--max-body", "2M", "--body-budget", "1M",
          message="--max-body exceeds --body-budget")
    fails("--max-body", "2M", "--client-budget", "1M",
          message="--max-body exceeds --client-budget")
    fails("--output-pause", "9M", message="--output-pause exceeds --max-output")
    fails("--max-calls", "0", message="invalid --max-calls")
    fails("--slots", "0", message="invalid --slots")
    fails("--slots", "65536", message="invalid --slots")
    fails("--chunk-timeout", "-1", message="invalid --chunk-timeout")
    for loops in ("256", "-1", "x", ""):
        fails("--loops", loops, message="invalid --loops")

    # A malformed listen address makes askai exit with status 2 before it
    # listens anywhere.  With a host name that does not resolve, askai fails
    # later, when it opens the listener, and exits with status 1.
    for addr, error in (("127.0.0.1:99999", errno.ERANGE),
                        ("127.0.0.1:65536", errno.ERANGE),
                        ("65536", errno.ERANGE),
                        ("tls:127.0.0.1:70000", errno.ERANGE),
                        ("127.0.0.1:abc", errno.EINVAL),
                        ("127.0.0.1:", errno.EINVAL),
                        ("127.0.0.1:-1", errno.EINVAL),
                        ("127.0.0.1:+80", errno.EINVAL),
                        ("127.0.0.1: 80", errno.EINVAL),
                        ("localhost", errno.EINVAL),
                        ("", errno.EINVAL),
                        ("[::1]:x", errno.EINVAL),
                        ("[::1]", errno.EINVAL),
                        ("[::1]:", errno.EINVAL),
                        ("[::1:0", errno.EINVAL),
                        ("[::1]]:0", errno.EINVAL),
                        ("[]:0", errno.EINVAL),
                        ("[:0", errno.EINVAL),
                        ("[localhost]:0", errno.EINVAL),
                        ("[127.0.0.1]:0", errno.EINVAL),
                        ("::1", errno.EINVAL),
                        ("[::1%]:0", errno.EINVAL),
                        ("x" * 256 + ":0", errno.EINVAL),
                        ("unix:", errno.EINVAL),
                        ("unix:/" + "x" * 107, errno.ENAMETOOLONG),
                        ("tls:unix:/x", errno.EPROTONOSUPPORT)):
        fails("-u", up.url(), "-l", addr,
              message=f"invalid --listen: {addr}: {os.strerror(error)}")
    # A name that cannot be resolved, and an IPv6 address with a zone, are
    # well formed, so they fail only as askai opens the listener.
    for addr in ("nonexistent.invalid:0", "[fe80::1%lo]:0", "fe80::1%lo:0"):
        run = subprocess.run([binary, "-u", up.url(), "-l", addr],
                             stdin=subprocess.DEVNULL, capture_output=True,
                             text=True, timeout=5, check=False)
        assert run.returncode == 1 and f"cannot listen on {addr}" in run.stderr, \
            (addr, run.stderr)

    # A Unix socket path of 107 bytes fits in sun_path.  askai accepts a port
    # with leading zeros and a port without a host.
    with tempfile.TemporaryDirectory() as d:
        assert len(d) < 100, d
        sock = f"{d}/" + "s" * (107 - len(d) - 1)
        with Askai(binary, "-u", up.url(), "-l", "127.0.0.1:00000", "-l", "0",
                   "-l", f"unix:{sock}") as ai:
            ai.wait_log(f"on unix:{sock}$")
            ports = [line for line in ai.log
                     if re.search(r"MCP endpoint http://127\.0\.0\.1:\d+/mcp$", line)]
            assert len(ports) == 2 and os.path.exists(sock), ai.log

    with Askai(binary, "-p", "google", "-m", "m",
               env={"GOOGLE_API_KEY": "fallback-key"}) as ai:
        _, _, body = ai.rpc("tools/list")
        assert body["result"]["tools"][0]["name"] == "ask_ai"
    with Askai(binary, "-p", "google", "--api", "interactions", "-m", "m",
               env={"GEMINI_API_KEY": "k"}) as ai:
        _, _, body = ai.rpc("tools/list")
        assert body["result"]["tools"][0]["name"] == "ask_ai"
    # With --url, the Interactions API is asked at that URL as it is given,
    # with the key in x-goog-api-key.
    with Askai(binary, "-p", "google", "--api", "interactions", "-m", "m",
               "-u", up.url("/v1beta/interactions"), "-k", "TEST_KEY",
               env={"TEST_KEY": "secret", "GEMINI_API_KEY": "must-not-leak"}) as ai:
        up.reply = lambda request: (200, ANSWERS["interactions"])
        assert text_of(ai.ask()) == "A"
        request = up.last()
        assert request["path"] == "/v1beta/interactions", request["path"]
        assert request["headers"]["x-goog-api-key"] == "secret", request["headers"]
        assert "authorization" not in request["headers"], request["headers"]
        assert request["body"]["model"] == "m", request["body"]
    up.reply = lambda request: (200, ANSWERS["openai"])

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

    # json-c also parses numbers that JSON does not allow, such as "1.",
    # "-.5" and "01.5".  askai sends each of them as a JSON number.
    with Askai(binary, "-u", up.url(), "-t", "1.",
               "-x", '{"top_p": 1., "a": [1.e1, -.5, {"b": 01.5}]}') as ai:
        assert text_of(ai.ask()) == "A"
        body = json.loads(up.last()["raw"], parse_constant=constant)
        assert [body[k] for k in ("temperature", "top_p", "a")] == \
            [1.0, 1.0, [10.0, -0.5, {"b": 1.5}]], body

    # A configuration file: a JSON object in strict JSON, without comments,
    # whose objects upstream, service, testing and debugging have the long
    # options as members, which the command line overrides.
    with tempfile.TemporaryDirectory() as d:
        path = f"{d}/askai.json"

        def write(text, mode=0o600):
            with open(path, "w") as f:
                f.write(text)
            os.chmod(path, mode)

        write("""{
            "upstream": {
                "url": "%s", "tool-name": "ask_file",
                "tool-description": "From the file.", "header": ["X-A: 1", "X-B: 2"],
                "temperature": 0.5, "extra": {"top_k": 5}, "max-tokens": 9
            },
            "service": {"man": true},
            "testing": {"test-fifo": false},
            "debugging": {}
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

        url = '"upstream": {"url": "%s"}' % up.url()
        for text, message in (
                ('{%s, "bogus": 1}' % url, f"{path}: unknown member bogus"),
                ('{%s, "config": "x"}' % url, "unknown member config"),
                ('{%s, "service": {"help": true}}' % url, "service: unknown member help"),
                ('{%s, "loops": 1}' % url, f"{path}: loops must be in service"),
                ('{"upstream": {"url": "%s", "verbose": true}}' % up.url(),
                 f"{path}: verbose must be in debugging"),
                ('{"url": "%s"}' % up.url(), f"{path}: url must be in upstream"),
                ('{%s, "testing": [1]}' % url, f"{path}: testing must be an object"),
                ('{%s, "service": {"loops": [1]}}' % url, f"{path}: invalid --loops: [1]"),
                ('{%s, "service": {"man": "yes"}}' % url, "invalid --man"),
                ('{%s, "service": {"max-body": "1X"}}' % url, "invalid --max-body: 1X"),
                ('{%s, "service": {"man": null}}' % url, "invalid --man: null"),
                ('{"upstream": {"url": "%s", "temperature": NaN}}' % up.url(),
                 f"{path}: invalid temperature: NaN"),
                ('{"upstream": {"url": "%s", "extra": {"a": [Infinity]}}}' % up.url(),
                 f'{path}: --extra contains NaN, an infinity or a number outside '
                 'the range of a double: {"a":[Infinity]}'),
                ('{"upstream": {"url": "%s", "header": [["X-A: 1", "X-B: 2"]]}}' % up.url(),
                 f'{path}: invalid --header: [["X-A: 1","X-B: 2"]]'),
                ('{"upstream": {"url": "ftp://x"}}', f"{path}: not an http(s) URL"),
                ('{%s, "tools": ["nope"]}' % url,
                 f'{path}: tools: askai has no tool "nope"; the tools are none'),
                ('{%s, "service": {"man": true}, "tools": {"client": [1]}}' % url,
                 f"{path}: tools: client: askai has no tool 1; the tools are man"),
                ('{%s, "tools": ["ask_ai"]}' % url,
                 f"{path}: tools: ask_ai is the ask tool, which is always on"),
                ('{%s, "tools": {"allow": [], "deny": []}}' % url,
                 f"{path}: tools: allow and deny together"),
                ('{%s, "tools": {"client": true, "deny": []}}' % url,
                 f"{path}: tools: allow and deny go into client and upstream"),
                ('{%s, "tools": {"client": true, "x": 1}}' % url,
                 f"{path}: tools: unknown member x"),
                ('{%s, "service": {"client-jobs": true}, "tools": {"deny": ["jobs"]}}' % url,
                 f"{path}: tools: background needs jobs"),
                ('{%s, "service": {"client-jobs": true}, '
                 '"tools": {"upstream": ["background"]}}' % url,
                 f"{path}: tools: upstream: background needs jobs"),
                ('{%s, "tools": 1}' % url,
                 f"{path}: tools: not true, false, a list or an object"),
                ('{%s, "tools": {"upstream": {"only": []}}}' % url,
                 f"{path}: tools: upstream: unknown member only"),
                ('{%s, "tools": {"deny": "man"}}' % url,
                 f"{path}: tools: not a list of tools"),
                ('{\n %s\n "debugging": {}\n}' % url,
                 f"{path}: 3:2: object value separator ',' expected"),
                ('{\n %s, // the upstream\n "debugging": {}\n}' % url,
                 f"{path}: 2:{len(url) + 4}: quoted object property name expected"),
                ('{\n %s, /* the upstream */\n "debugging": {}\n}' % url,
                 f"{path}: 2:{len(url) + 4}: quoted object property name expected"),
                ('{\n %s,\n "debugging": {},\n}' % url, f"{path}: 4:1: unexpected character"),
                ('[1]', f"{path}: 1:1: not one JSON object"),
                ('{%s} x' % url, f"{path}: 1:{len(url) + 4}: unexpected character"),
                ('{%s}\0x' % url, f"{path}: 1:{len(url) + 3}: not one JSON object"),
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


def check_allow(binary, up):
    """The member tools of the configuration file: the tools that it allows
    clients and the upstream, of those that options turn on, whatever the
    roles name; and what the report shows of it."""
    def lent(ai):
        count = len(up.requests)
        assert text_of(ai.ask("Q")) == "A"
        return [t["function"]["name"] for t in up.requests[count]["body"].get("tools", [])]

    with tempfile.TemporaryDirectory() as d:
        path = f"{d}/askai.json"
        on = {"url": up.url(), "upstream-tools": True, "notes": f"{d}/notes",
              "man": True, "client-jobs": True}
        jobs = ["background", "jobs", "wait", "timer"]
        for tools, clients, turns in (
                (True, ["ask_ai", "man", "notes", *jobs],
                 ["man", "notes", *jobs, "compact"]),
                (False, ["ask_ai"], []),
                (["man"], ["ask_ai", "man"], ["man"]),
                ({"allow": ["man", "compact"]}, ["ask_ai", "man"], ["man", "compact"]),
                ({"deny": ["man", "compact"]}, ["ask_ai", "notes", *jobs],
                 ["notes", "wait", "timer"]),
                ({}, ["ask_ai", "man", "notes", *jobs],
                 ["man", "notes", *jobs, "compact"]),
                ({"client": {"allow": ["notes"]}, "upstream": {"deny": ["notes"]}},
                 ["ask_ai", "notes"], ["man", *jobs, "compact"]),
                ({"upstream": False}, ["ask_ai", "man", "notes", *jobs], [])):
            with open(path, "w") as f:
                json.dump(grouped(on | {"tools": tools}), f)
            os.chmod(path, 0o600)
            with Askai(binary, "-c", path) as ai:
                listed = ai.rpc("tools/list")[2]["result"]["tools"]
                assert [t["name"] for t in listed] == clients, (tools, listed)
                assert lent(ai) == turns, (tools, up.requests[-1]["body"])
                # The default description of the ask tool, and its argument
                # thread, follow what the turns get.
                ask = listed[0]
                assert ("It may use some of this server's tools" in ask["description"],
                        "It keeps notes in a thread" in ask["description"],
                        "thread" in ask["inputSchema"]["properties"]) == \
                    (bool(turns), "notes" in turns, "notes" in turns), (tools, ask)
                if turns and "notes" not in turns:
                    _, _, body = ai.rpc("tools/call", {"name": "ask_ai", "arguments": {
                        "prompt": "Q", "thread": "0" * 32}})
                    assert "No notes are kept for you here" in text_of(body["result"]), \
                        (tools, body)
                report = [item.split() for item in ai.listed("built-in tools for clients:")]
                off = [item[0] for item in report if item[1:] == ["off", "(tools)"]]
                assert off == [t for t in ["man", "git", "notes", *jobs]
                               if t not in clients and t != "git"], (tools, report)

        # A role may name a tool that the member denies clients: no client
        # gets it, and the warning about the tools that no role lists leaves
        # it out.  The role's tools still lend it to the upstream.
        with open(path, "w") as f:
            json.dump(grouped(on | {"tools": {"client": {"deny": ["man"]}},
                                    "roles": {"r": {"tools": ["ask_ai", "man", "notes"]}},
                                    "anonymous": {"loopback": "r"}}), f)
        os.chmod(path, 0o600)
        with Askai(binary, "-c", path) as ai:
            listed = ai.rpc("tools/list")[2]["result"]["tools"]
            assert [t["name"] for t in listed] == ["ask_ai", "notes"], listed
            assert lent(ai) == ["man", "notes", *jobs, "compact"]
            assert [item.split()[-1] for item in ai.listed("role r:")][:2] == \
                ["ask_ai", "notes"], ai.log
            assert not any("no role lists" in line for line in ai.log), ai.log

    print("tools: allowed and denied for clients and the upstream, whatever the "
          "roles name, and shown in the report: PASS")


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
                json.dump(grouped({"url": up.url(), "man": True} | policy), f)
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
                # A modern client gets the same list, which no cache may
                # give to another caller.
                status, _, body = ai.modern("tools/list", headers=auth(token))
                assert status == 200 and body["result"]["cacheScope"] == "private" and \
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
                ({"roles": {"r": {"upstream": "git"}}}, "role r: upstream must be"),
                ({"roles": {"r": {"tools": ["ask_ai"], "upstream": ["ask_ai"]}}},
                 'role r: askai offers no tool "ask_ai" to the upstream'),
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
            json.dump(grouped({"url": up.url(), "roles": {"r": {"tools": ["man"]}}}), f)
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
            text = json.dumps(grouped({"url": up.url(), "man": True} | options | policy))
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
            reload(ai, "configuration reloaded: policy")
            assert (tools(ai, "t1"), tools(ai, "t2"), tools(ai, None)) == \
                (["man"], ["ask_ai"], ["ask_ai"])
            # A wrong file, or a policy that does not hold, leaves the last.
            with open(path, "a") as f:
                f.write("x")
            reload(ai, "configuration not reloaded$")
            write({"roles": {"a": full}, "anonymous": {"remote": "z"}})
            assert 'anonymous: remote: no role "z"' in \
                reload(ai, "no role").strip() + "".join(ai.log)
            ai.wait_log("configuration not reloaded")
            assert (tools(ai, "t1"), tools(ai, "t2")) == (["man"], ["ask_ai"])
            # Other options keep, until a restart.
            write(first, **{"tool-description": "New."})
            reload(ai, "configuration reloaded")
            ai.wait_log(r"^askai: warning: other changed options need a restart$")
            _, _, body = ai.rpc("tools/list", headers=auth("t1"))
            assert body["result"]["tools"][0]["description"] != "New.", body
            # The roles name the tools that askai runs, not those the file
            # now asks for: man, dropped from the file, runs until a restart,
            # and a new name for the ask tool is no tool yet.
            write(first, man=False)
            reload(ai, "configuration reloaded")
            ai.wait_log(r"^askai: warning: other changed options need a restart$")
            assert tools(ai, "t1") == ["ask_ai", "man"]
            # So does a change of the member tools, which a reload checks.
            write(first, tools={"deny": ["man"]})
            reload(ai, "configuration reloaded")
            ai.wait_log(r"^askai: warning: other changed options need a restart$")
            assert tools(ai, "t1") == ["ask_ai", "man"]
            write(first, tools=["nope"])
            assert 'tools: askai has no tool "nope"; the tools are man' in \
                reload(ai, "has no tool").strip() + "".join(ai.log)
            ai.wait_log("configuration not reloaded")
            assert tools(ai, "t1") == ["ask_ai", "man"]
            write({"roles": {"a": {"tools": ["ask_twin"]}}}, **{"tool-name": "ask_twin"})
            assert 'askai offers no tool "ask_twin"; the tools are ask_ai, man\n' in \
                reload(ai, "offers no tool") + "".join(ai.log)
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


def check_logs(binary, up):
    """As askai starts, before its endpoints, it logs lists with one item a
    line: the upstream, each built-in tool and whether it is on, the git
    repositories, the plugins and the policy, with the tools that each role
    lends to the upstream and a warning with the tools that no role lists;
    a reload logs the policy again.  The lines have no rate limit.  A role that names a tool that
    askai does not offer stops askai with the list of the tools that it
    offers.  A client's tool call that fails or is refused is logged with
    its error.  With --verbose, askai logs each tool call of a client with
    its arguments, the job that a call of background starts, the start of
    each ask call, each upstream turn, each tool call of a turn with the
    error of a failure, each compaction and each notice about the context;
    of a note's text and of compact's summary only the size.  No line has a token, an API key, a password or the query of
    the URL."""
    def told(ai):
        return [line.removeprefix("askai: ").rstrip("\n") for line in ai.log]

    def has(ai, *lines):
        got = told(ai)
        for line in lines:
            assert line in got, (line, got)

    def run(*args, env=None):
        return subprocess.run([binary, "-l", "127.0.0.1:0", *args], capture_output=True,
                              text=True, timeout=5, env={
                                  k: v for k, v in os.environ.items()
                                  if k not in KEY_VARS} | (env or {}))

    script = []
    scripted = lambda r: script.pop(0)(r) if script else (200, turn_stream(text="A"))
    up.reply = scripted
    with tempfile.TemporaryDirectory() as d:
        d = os.path.realpath(d)
        repo = f"{d}/proj"
        notes = f"{d}/notes"
        subprocess.run(["git", "init", "-q", repo], check=True)

        # The built-in tools but the ask tool are off, and so are the tools
        # of the upstream.  askai's own policy applies, without ASKAI_TOKEN.
        # test_task, a test aid, is left out while it is off.
        with Askai(binary, "-u", up.url()) as ai:
            assert ai.listed("upstream:") == [
                "provider  none", "API       chat", "model     none",
                "context   from llama-server", f"URL       {up.url()}"], told(ai)
            assert ai.listed("built-in tools for clients:") == [
                "ask_ai      on", "man         off (--man)", "git         off (--git PATH)",
                "notes       off (--notes DIR)"] + [
                f"{name:<10}  off (--client-jobs)"
                for name in ("background", "jobs", "wait", "timer")], told(ai)
            has(ai, "plugins: none", "upstream tools: off (--upstream-tools)")
            assert ai.listed("policy: built in, without ASKAI_TOKEN") == [
                "loopback        all tools and metrics", "remote          all tools",
                "unix            all tools and metrics"], told(ai)
            assert not any(line.startswith(("git repositories", "notes directory"))
                           for line in told(ai)), told(ai)
        # With --slots and no tools for the upstream, askai asks no /props,
        # and the context is unknown.
        with Askai(binary, "-u", up.url(), "--slots", "2") as ai:
            assert ai.listed("upstream:")[3] == "context   unknown", told(ai)

        # Without --verbose, a failed call is still logged, with its error
        # but without its arguments.
        with Askai(binary, "-u", up.url(), "--notes", notes) as ai:
            ai.rpc("tools/call", {"name": "notes", "arguments": {
                "action": "read", "file": "SECRETFILE", "thread": "x"}})
            line = ai.wait_log(r"^askai: tool call notes from anonymous failed ")
            assert re.fullmatch(
                r'askai: tool call notes from anonymous failed after \d+\.\d s \(90 bytes\); '
                r'error "The thread argument must be the id of a thread, as an add named '
                r'it: 32 hexadecimal digits\."\n', line), line
            assert "SECRETFILE" not in "".join(ai.log), ai.log

        # A provider by name, a context of unknown size, and no API key in
        # the log.
        with Askai(binary, "-p", "openai", "-u", up.url(), "-m", "M",
                   env={"OPENAI_API_KEY": "sk-SECRETKEY"}) as ai:
            assert {"provider  openai", "context   unknown"} <= set(ai.listed("upstream:")), \
                told(ai)
            assert "SECRETKEY" not in "".join(ai.log), ai.log

        # The built-in tools are on, each line with its details.  The log
        # has no user, password or query of the URL, and no token.  The
        # call of a client with ASKAI_TOKEN names that identity.
        url = up.url().replace("http://", "http://user:PASSWORD@") + "?key=SECRET"
        with Askai(binary, "-u", url, "-m", "M", "--context", "8000", "--tool-name",
                   "bro", "--man", "--test-task", "--git", repo, "--notes", notes,
                   "--upstream-tools", "--verbose", env={"ASKAI_TOKEN": "tok-31415"}) as ai:
            assert ai.listed("upstream:") == [
                "provider  none", "API       chat", "model     M", "context   8000 tokens",
                f"URL       {up.url()}"], told(ai)
            assert ai.listed("built-in tools for clients:") == [
                f"{name:<10}  on" for name in ("bro", "man", "test_task", "git", "notes")
            ] + [f"{name:<10}  off (--client-jobs)"
                 for name in ("background", "jobs", "wait", "timer")], told(ai)
            has(ai, f"notes directory: {notes}")
            assert ai.listed("git repositories:") == [f"proj  {repo}"], told(ai)
            assert ai.listed("policy: built in, with ASKAI_TOKEN") == [
                "clients with the token  all tools and metrics",
                "other clients           refused",
                "upstream tools          man"] + [
                f"{'':22}  {name}" for name in ("git", "notes", "compact", "background",
                                                 "jobs", "wait", "timer")], told(ai)
            assert ai.rpc("tools/call", {"name": "bro", "arguments": {"prompt": "Q"}},
                          headers={"Authorization": "Bearer tok-31415"})[0] == 200
            ai.wait_log(r"^askai: tool call bro from identity ASKAI_TOKEN answered ")
            log = "".join(ai.log)
            assert not any(word in log for word in ("PASSWORD", "SECRET", "tok-31415")), log

        # askai logs each role of the file's policy with the tools that its
        # ask calls lend to the upstream, the identities with their roles, the
        # groups and the anonymous clients, and a warning for man, which no
        # role lists.
        path = f"{d}/askai.json"

        def write(**members):
            with open(f"{path}.new", "w") as f:
                json.dump(grouped({"url": up.url(), "man": True, "git": repo} | members), f)
            os.chmod(f"{path}.new", 0o600)
            os.rename(f"{path}.new", path)
        digest = "sha256:" + hashlib.sha256(b"ci-token").hexdigest()
        roles = {"admin": {"tools": ["ask_ai", "git"], "metrics": True},
                 "user": {"tools": ["ask_ai"], "upstream": []}}
        policy = {"roles": roles,
                  "identities": {"alice": {"role": "admin", "tokens": ["env:ALICE"]},
                                 "ci": {"role": "user", "tokens": [digest]}},
                  "unix-groups": {"0": "admin"},
                  "anonymous": {"loopback": "user"}}
        alice = {"Authorization": "Bearer alice-token"}
        write(**policy, **{"upstream-tools": True})
        with Askai(binary, "-c", path, "--verbose", env={"ALICE": "alice-token"}) as ai:
            has(ai, "policy: from the configuration file")
            assert ai.listed("role admin:") == [
                "tools           ask_ai", "                git",
                "upstream tools  git"] + [
                f"{'':14}  {name}" for name in ("compact", "background", "jobs", "wait",
                                                 "timer")] + [
                "metrics         yes"], told(ai)
            assert ai.listed("role user:") == [
                "tools           ask_ai", "upstream tools  none", "metrics         no"
            ], told(ai)
            assert ai.listed("identities:") == ["alice  role admin", "ci     role user"]
            assert ai.listed("Unix groups:") == ["0  role admin"]
            assert ai.listed("anonymous clients:") == [
                "loopback  role user", "remote    refused", "unix      refused"]
            assert ai.listed("warning: no role lists these tools:") == ["man"], told(ai)
            log = "".join(ai.log)
            assert "alice-token" not in log and digest[7:] not in log, log
            # An identity's call names the identity, and the call of an
            # anonymous client on loopback names its kind and role.
            assert ai.rpc("tools/call", {"name": "ask_ai", "arguments": {"prompt": "Q"}},
                          headers=alice)[0] == 200
            ai.wait_log(r"^askai: tool call ask_ai from identity alice answered after ")
            assert ai.rpc("tools/call", {"name": "ask_ai", "arguments": {"prompt": "Q"}},
                          headers={"Authorization": "Bearer ci-token"})[0] == 200
            ai.wait_log(r"^askai: tool call ask_ai from identity ci answered after ")
            ai.ask("Q")
            ai.wait_log(r"^askai: tool call ask_ai from anonymous loopback \(role user\) "
                        r"answered after ")
            # A call of a tool that the role does not grant, or that askai
            # does not offer, is logged as refused, the client's name of an
            # unknown tool as a JSON string.
            ai.rpc("tools/call", {"name": "man", "arguments": {"name": "ls"}})
            ai.wait_log(r"^askai: tool call man from anonymous loopback \(role user\) "
                        r"refused: the role does not grant it$")
            ai.rpc("tools/call", {"name": "no\nsuch", "arguments": {}})
            ai.wait_log(r'^askai: tool call "no\\nsuch" from anonymous loopback \(role '
                        r'user\) refused: askai offers no such tool$')
            # A turn's call of a tool that its ask call does not lend, or that
            # askai does not offer, is logged as refused with its arguments.
            script[:] = [lambda r: (200, turn_stream([("a", "man", '{"name":"ls"}'),
                                                      ("b", "no\nsuch", "{}")]))]
            assert ai.rpc("tools/call", {"name": "ask_ai", "arguments": {"prompt": "Q"}},
                          headers=alice)[0] == 200
            ai.wait_log(r'^askai: ask call \d+\.\d+ turn 1: tool call "man" refused: not '
                        r'lent to this ask call; arguments \{"name":"ls"\}$')
            ai.wait_log(r'^askai: ask call \d+\.\d+ turn 1: tool call "no\\nsuch" refused: '
                        r'askai offers no such tool; arguments \{\}$')
            # A call that runs across a reload still names its client, which
            # it copied from the old policy.
            up.reply = lambda r: (200, turn_stream(text="A"), 1.5)
            up.arrived.clear()
            slow = threading.Thread(target=lambda: ai.rpc(
                "tools/call", {"name": "ask_ai", "arguments": {"prompt": "R"}},
                headers=alice))
            slow.start()
            assert up.arrived.wait(5)
            # A reload logs the new policy.
            write(**(policy | {"roles": roles | {"admin": {"tools": ["ask_ai", "man"]}}}),
                  **{"upstream-tools": True})
            ai.log.clear()
            ai.proc.send_signal(signal.SIGHUP)
            ai.wait_log("configuration reloaded")
            ai.wait_list("role admin:", [
                "tools           ask_ai", "                man",
                "upstream tools  man"] + [
                f"{'':14}  {name}" for name in ("compact", "background", "jobs", "wait",
                                                 "timer")] + [
                "metrics         no"])
            ai.wait_list("warning: no role lists these tools:", ["git"])
            slow.join(5)
            ai.wait_log(r"^askai: tool call ask_ai from identity alice answered after ")
            up.reply = scripted

        # A tool that a role lends to the upstream is listed, and notes,
        # which every ask call lends, needs no role.
        write(roles={"user": {"tools": ["ask_ai"], "upstream": ["man"]}},
              anonymous={"loopback": "user"}, notes=notes, **{"upstream-tools": True})
        with Askai(binary, "-c", path) as ai:
            assert ai.listed("role user:") == [
                "tools           ask_ai", "upstream tools  man"] + [
                f"{'':14}  {name}" for name in ("notes", "compact", "background", "jobs",
                                                 "wait", "timer")] + [
                "metrics         no"], told(ai)
            assert ai.listed("warning: no role lists these tools:") == ["git"], told(ai)

        # The report is not rate-limited.
        write(roles={f"r{i:02}": {"tools": ["ask_ai"]} for i in range(40)})
        with Askai(binary, "-c", path) as ai:
            assert [line for line in told(ai) if line.startswith("role r")] == \
                [f"role r{i:02}:" for i in range(40)], told(ai)
            assert all(ai.listed(f"role r{i:02}:") == [
                "tools           ask_ai", "metrics         no"] for i in range(40)), told(ai)

        # A role that names a tool askai does not offer stops askai, and the
        # error lists the tools that askai offers or lends.
        for members, message in (
                ({"roles": {"r": {"tools": ["godbolt"]}}, "upstream-tools": True},
                 'role r: askai offers no tool "godbolt"; the tools are ask_ai, man, git'),
                ({"roles": {"r": {"upstream": ["ask_ai"]}}, "upstream-tools": True},
                 'role r: askai offers no tool "ask_ai" to the upstream; the tools that '
                 'it lends are man, git'),
                ({"roles": {"r": {"upstream": ["git"]}}},
                 'role r: askai offers no tool "git" to the upstream; it lends no tool')):
            write(**members)
            proc = run("-c", path)
            assert proc.returncode == 2 and f"{path}: {message}\n" in proc.stderr, \
                (members, proc.stderr)

        # With --verbose, the line of a client's call of notes has only the
        # size of the text.  An ask call logs its start, its turns, the calls
        # of its turns, the notices about its context, its compactions, and
        # its end with its arguments cut to 200 bytes.
        with Askai(binary, "-u", up.url(), "--verbose", "--upstream-tools", "--notes",
                   notes, "--context", "8000") as ai:
            status, _, body = ai.rpc("tools/call", {"name": "notes", "arguments": {
                "action": "add", "file": "a.md", "text": "NOTETEXT" * 10}})
            assert status == 200 and not body["result"].get("isError"), body
            ai.wait_log(r'^askai: tool call notes from anonymous answered after \d+\.\d s '
                        r'\(\d+ bytes\); arguments \{"action":"add","file":"a\.md",'
                        r'"text":"\(80 bytes\)"\}$')
            # The arguments are JSON as json-c writes it, and a failure has its
            # text.
            odd = {"action": "read", "file": "a\"b\\c\né", "thread": "x",
                   "n": [1, 2.5, True, None, {"k": "v"}, []], "o": {}}
            status, _, body = ai.rpc("tools/call", {"name": "notes", "arguments": odd})
            assert status == 200 and body["result"].get("isError"), body
            line = ai.wait_log(r"^askai: tool call notes from anonymous failed ")
            assert line.endswith("; arguments " + json.dumps(odd, separators=(",", ":"),
                                                             ensure_ascii=False) +
                                 '; error "The thread argument must be the id of a '
                                 'thread, as an add named it: 32 hexadecimal '
                                 'digits."\n'), line
            script[:] = [
                lambda r: (200, turn_stream([("a", "notes", json.dumps(
                    {"action": "add", "file": "n.md", "text": "NOTETEXT"}))],
                    used=(5500, 10))),
                lambda r: (200, turn_stream([("b", "compact", json.dumps(
                    {"summary": "SUMMARYTEXT"}))], used=(1000, 10))),
                lambda r: (200, turn_stream([("c", "notes", '{"action":"list"}')],
                                            used=(7990, 10))),
            ]
            assert ai.ask("x" * 300)["content"][0]["text"] == "A"
            start = ai.wait_log(r'^askai: ask call \d+\.\d+ from anonymous: prompt 300 '
                                r'bytes, thread [0-9a-f]{32} \(new\), "x{100}"\.\.\.$')
            call = re.escape(re.search(r"ask call (\d+\.\d+)", start).group(1))
            for pattern in (
                    r'turn 1: answered, HTTP 200 after \d+\.\d s, 5510 of 8000 tokens '
                    r'\(68 %\), stop reason "tool_calls", 1 tool call, 0 bytes of text',
                    r'turn 1: tool call notes answered after \d+\.\d s \(\d+ bytes\); '
                    r'arguments \{"action":"add","file":"n\.md","text":"\(8 bytes\)"\}',
                    r"turn 1: context notice, 6\d % full, \d+ of 8000 tokens: a request "
                    r"to call compact",
                    r"turn 2: compacted by the upstream's compact call, summary 11 bytes",
                    r"turn 3: context full, compacted by askai",
                    r'turn 4: answered, HTTP 200 after \d+\.\d s, tokens unknown, stop '
                    r'reason "stop", 0 tool calls, 1 byte of text'):
                ai.wait_log(f"^askai: ask call {call} " + pattern + "$")
            ai.wait_log(r"^askai: tool call ask_ai from anonymous answered after \d+\.\d s "
                        rf"\(1 byte\); ask call {call}, 4 turns, 3 tool calls; arguments "
                        r'\{"prompt":"x{189}\.\.\.$')
            log = "".join(ai.log)
            assert "NOTETEXT" not in log and "SUMMARYTEXT" not in log, log
            # The cut is never inside an escape, and 200 bytes are not cut.
            for prompt, shown in (('"' * 300, '\\"' * 94), ('\x01' * 300, '\\u0001' * 31),
                                  ('x' * 187, None)):
                ai.log.clear()
                ai.ask(prompt)
                line = ai.wait_log(r"^askai: tool call ask_ai from anonymous answered ")
                got = line.rstrip("\n").split("; arguments ", 1)[1]
                assert got == ('{"prompt":"' + "x" * 187 + '"}' if shown is None else
                               '{"prompt":"' + shown + "..."), got
            # A failed compact call logs the size of its summary.
            script[:] = [lambda r: (200, turn_stream([("d", "compact", json.dumps(
                {"summary": "SUMMARYTEXT" * 400_000}))]))]
            ai.log.clear()
            assert ai.ask("Q")["content"][0]["text"] == "A"
            ai.wait_log(r'^askai: ask call \d+\.\d+ turn 1: tool call compact failed after '
                        r'\d+\.\d s \(\d+ bytes\); arguments \{"summary":"\(4400000 bytes\)"\}; '
                        r'error ".+"$')
            assert "SUMMARYTEXT" not in "".join(ai.log), ai.log
            # A failed call of a turn has its error, and arguments that are no
            # JSON object say so.
            script[:] = [lambda r: (200, turn_stream([("e", "timer", '{"seconds":0}'),
                                                      ("f", "timer", "[1]")]))]
            ai.log.clear()
            ai.ask("Q")
            ai.wait_log(r'^askai: ask call \d+\.\d+ turn 1: tool call timer failed after '
                        r'\d+\.\d s \(\d+ bytes\); arguments \{"seconds":0\}; error "The '
                        r'seconds argument must be a whole number from 1 to 3600\."$')
            ai.wait_log(r'^askai: ask call \d+\.\d+ turn 1: tool call timer failed after '
                        r'\d+\.\d s \(\d+ bytes\); arguments not a JSON object; error "Your '
                        r'arguments were not a JSON object, so the call was not run\."$')
            # An answer that is not streamed, such as an HTTP error's body, has
            # no tokens, stop reason or tool calls.
            script[:] = [lambda r: (401, {"error": {"message": "no"}})]
            ai.log.clear()
            ai.ask("Q")
            ai.wait_log(r"^askai: ask call \d+\.\d+ turn 1: failed, HTTP 401 after \d+\.\d s, "
                        r"not streamed, \d+ bytes of body$")
            ai.wait_log(r'^askai: tool call ask_ai from anonymous failed after \d+\.\d s '
                        r'\(\d+ bytes\); ask call \d+\.\d+, 1 turn, 0 tool calls; arguments '
                        r'\{"prompt":"Q"\}; error "Upstream error \(HTTP 401\): no"$')
            # A new thread, a reminder to write notes, and the thread resumed.
            script[:] = [lambda r: (200, turn_stream([("a", "notes", '{"action":"list"}')],
                                                     used=(2100, 10)))]
            ai.log.clear()
            ai.ask("Q")
            start = ai.wait_log(r'^askai: ask call \d+\.\d+ from anonymous: prompt 1 byte, '
                                r'thread [0-9a-f]{32} \(new\), "Q"$')
            call = re.escape(re.search(r"ask call (\S+) ", start).group(1))
            thread = re.search(r"thread (\w{32})", start).group(1)
            ai.wait_log(rf"^askai: ask call {call} turn 1: context notice, 26 % full, \d+ of "
                        r"8000 tokens: a reminder to write notes$")
            ai.log.clear()
            ai.ask("Q", thread=thread)
            ai.wait_log(rf'^askai: ask call \d+\.\d+ from anonymous: prompt 1 byte, thread '
                        rf'{thread} \(resumed\), "Q"$')

        # Without notes an ask call has no thread.  A call that the turns
        # stop at --max-turns fails without a text, so its line has no
        # error.
        with Askai(binary, "-u", up.url(), "--verbose", "--man", "--upstream-tools",
                   "--max-turns", "2") as ai:
            script[:] = [lambda r: (200, turn_stream([("a", "man", '{"name":"ls"}')]))] * 2
            ai.ask("Q")
            ai.wait_log(r'^askai: ask call \d+\.\d+ from anonymous: prompt 1 byte, no '
                        r'thread, "Q"$')
            ai.wait_log(r'^askai: tool call ask_ai from anonymous failed after \d+\.\d s '
                        r'\(0 bytes\); ask call \d+\.\d+, 2 turns, 1 tool call, stopped at '
                        r'--max-turns; arguments \{"prompt":"Q"\}$')

        # With --client-jobs, the four tools of jobs are on.  A call of
        # background logs the job that it starts, and the line of the job's
        # end names the job and has the arguments of the job's call.
        with Askai(binary, "-u", up.url(), "--verbose", "--client-jobs") as ai:
            assert ai.listed("built-in tools for clients:")[-4:] == [
                f"{name:<10}  on" for name in ("background", "jobs", "wait", "timer")]
            status, _, body = ai.rpc("tools/call", {"name": "background", "arguments": {
                "tool": "ask_ai", "arguments": {"prompt": "Q"}}})
            assert status == 200 and not body["result"].get("isError"), body
            jid = re.search(r"Started job (\w{32})\.", text_of(body["result"])).group(1)
            ai.wait_log(rf"^askai: tool call background from anonymous became job {jid}, "
                        r"a call of ask_ai$")
            ai.wait_log(r"^askai: tool call ask_ai from anonymous answered after \d+\.\d s "
                        rf"\(1 byte\); job {jid}; ask call \d+\.\d+, 1 turn, 0 tool calls; "
                        r'arguments \{"prompt":"Q"\}$')
    up.reply = lambda request: (200, ANSWERS["openai"])

    print("logs: the upstream, tools, plugins and policy as askai starts, the "
          "tools that an error lists, refused and failed calls, the lines of "
          "--verbose: PASS")


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
                json.dump(grouped({"url": up.url(), "man": True} | policy), f)
            os.chmod(path, 0o600)

        roles = {"a": {"tools": ["ask_ai", "man"], "metrics": True},
                 "g": {"tools": ["man"]}, "t": {"tools": ["ask_ai"]}}
        # askai's own policy: everything, as on loopback.
        with Askai(binary, "-u", up.url(), "--man", "-l", f"unix:{sock}") as ai:
            assert names(uds_rpc(sock)) == ["ask_ai", "man"]
            assert ai.rpc("ping")[0] == 200
            assert any(f"unix:{sock}" in line for line in ai.log), ai.log
            # A Unix socket's client without an identity is its Unix user.  A
            # notifications/cancelled from an anonymous TCP client does not
            # end the user's call, and one from another connection of the
            # same user over the socket does.
            hold = up.reply
            up.reply = lambda request: (200, ANSWERS["openai"], 1.0)
            notification = json.dumps({"jsonrpc": "2.0", "method": "notifications/cancelled",
                                       "params": {"requestId": 1}}).encode()
            for over_unix, expected in ((False, 200), (True, 202)):
                up.arrived.clear()
                results = []
                thread = threading.Thread(target=lambda: results.append(uds_rpc(
                    sock, "tools/call", {"name": "ask_ai", "arguments": {"prompt": "Q"}})))
                thread.start()
                assert up.arrived.wait(5)
                if over_unix:
                    with socket.socket(socket.AF_UNIX) as conn:
                        conn.settimeout(10)
                        conn.connect(sock)
                        conn.sendall(b"POST /mcp HTTP/1.1\r\nContent-Type: application/json\r\n"
                                     b"Content-Length: %d\r\n\r\n" % len(notification) +
                                     notification)
                        assert Replies(conn).next()[0] == 202
                else:
                    assert ai.rpc("notifications/cancelled", {"requestId": 1},
                                  rpc_id=None)[0] == 202
                thread.join(5)
                assert results[0][0] == expected, (over_unix, results)
            up.reply = hold
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

        # The log names the client of a call over the socket: its identity,
        # the role of its group, or its kind and role if anonymous.
        for policy, who, end in (
                ({"identities": {"me": {"role": "a", "unix-users": [me]}}}, "identity me",
                 "answered "),
                ({"unix-groups": {group: "g"}}, "role g",
                 "refused: the role does not grant it$"),
                ({"anonymous": {"unix": "t"}}, r"anonymous unix \(role t\)", "answered ")):
            write(roles=roles, **policy)
            with Askai(binary, "-c", path, "-l", f"unix:{sock}", "--verbose") as ai:
                assert uds_rpc(sock, "tools/call", {"name": "ask_ai", "arguments": {
                    "prompt": "Q"}})[0] == 200
                ai.wait_log(rf"^askai: tool call ask_ai from {who} {end}")

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
        assert run.returncode == 2 and "invalid --listen: unix:" in run.stderr, run
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
            ai.wait_log("configuration reloaded: policy and TLS certificate$")
            two = tls_client(crt)
            assert ai.rpc("ping", conn=https(two, port))[0] == 200
            assert refused(one, port)
            assert ai.rpc("ping", conn=old)[0] == 200
            os.replace(tls_files(d, "three")[1], key)
            ai.log.clear()
            ai.proc.send_signal(signal.SIGHUP)
            ai.wait_log("configuration not reloaded$")
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
              message="invalid --listen: tls:unix:/nonexistent/askai: " +
                      os.strerror(errno.EPROTONOSUPPORT))
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
        ai.wait_log(r"^askai: upstream /props: cannot look up ")
        ai.wait_log(r"^askai: upstream slots: 4 \(the default\)$")

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

    # Ctrl-C does the same.  askai keeps a signal ignored that it starts
    # with, and the tests may run with SIGINT ignored, as a shell sets it for
    # a job in the background.  So askai starts with SIGINT at its default
    # action, and a failed check does not leave askai running.
    ai = Askai(binary, "-u", up.url(), default=(signal.SIGINT,))
    try:
        assert ai.rpc("ping")[0] == 200
        ai.proc.send_signal(signal.SIGINT)
        assert ai.proc.wait(5) == 0
    finally:
        if ai.proc.poll() is None:
            ai.proc.kill()
            ai.proc.wait()

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
        "interactions": {"status": "completed", "steps": [
            {"type": "thought", "signature": "sig",
             "summary": [{"type": "text", "text": "hmm"}]},
            {"type": "model_output", "content": [{"type": "text", "text": text}]}]},
    }
    errors = {
        "openai": (None, {"error": {"message": "boom"}}),
        "responses": ("error", {"type": "error", "code": "x", "message": "boom"}),
        "anthropic": ("error", {"type": "error", "error": {
            "type": "overloaded_error", "message": "boom"}}),
        "gemini": (None, {"error": {"code": 500, "message": "boom"}}),
        "interactions": ("error", {"event_type": "error", "error": {
            "code": "api_error", "message": "boom"}}),
    }
    paths = {"openai": "/v1/chat/completions", "responses": "/v1/responses",
             "anthropic": "/v1/messages", "gemini": "/v1beta/models/m:generateContent",
             "interactions": "/v1beta/interactions"}
    # Stop reasons longer than most, of answers with no text.
    empty = {"openai": {"choices": [{"message": {"content": ""},
                                     "finish_reason": "content_filter"}]},
             "responses": {"status": "incomplete", "output": [],
                           "incomplete_details": {"reason": "content_filter"}},
             "anthropic": {"content": [],
                           "stop_reason": "model_context_window_exceeded"},
             "gemini": {"candidates": [{"content": {"parts": []},
                                        "finishReason": "IMAGE_PROHIBITED_CONTENT"}]},
             "interactions": {"status": "failed", "steps": []}}
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
            if protocol == "interactions":
                # The answer is the text items of the model_output steps.
                up.reply = lambda request: (200, {"status": "completed", "steps": [
                    {"type": "code_execution_result",
                     "content": [{"type": "text", "text": "no"}]},
                    {"type": "model_output", "content": [
                        {"type": "text", "text": "A"},
                        {"type": "text_annotation", "text": "no"},
                        {"type": "text", "text": "B"}]}]})
                assert text_of(ai.ask()) == "AB", protocol
                # interaction.completed ends the answer even without a status.
                up.plain = False
                up.reply = lambda request: (200, Stream(wire=sse_wire([
                    (n, {"event_type": n, "interaction": {"id": "v1_x"}})
                    if n == "interaction.completed" else (n, d)
                    for n, d in sse_of(ANSWERS["interactions"])])))
                assert text_of(ai.ask()) == "A", protocol
                up.plain = True
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
    for props, why in (({"total_slots": "2"}, "no total_slots"), (None, "HTTP 404")):
        up.props = props
        with Askai(binary, "-u", up.url()) as ai:
            ai.wait_log(f"^askai: upstream /props: {why}$")
            ai.wait_log(r"^askai: upstream slots: 4 \(the default\)$")
    gets = up.gets
    with Askai(binary, "-u", up.url(), "--slots", "3") as ai:
        ai.wait_log(r"upstream slots: 3 \(--slots\)")
    # Only a server that may be llama-server is asked: one that no provider
    # but llama names, and that has no Gemini API.
    for args in (("--api", "generate", "-u", up.url("/v1beta/models/m")),
                 ("--api", "interactions", "-u", up.url("/v1beta/interactions")),
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
        ai.wait_log(r"^askai: upstream /props: Operation timed out")
        ai.wait_log(r"^askai: upstream slots: 4 \(the default\)$")
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
        "interactions": {"status": "completed", "steps": [
            {"type": "thought", "summary": [{"type": "text", "text": "hmm"}]},
            {"type": "model_output", "content": [{"type": "text", "text": "xyz"}]}]},
    }
    paths = {"openai": "/v1/chat/completions", "responses": "/v1/responses",
             "anthropic": "/v1/messages", "gemini": "/v1beta/models/m:generateContent",
             "interactions": "/v1beta/interactions"}
    runs = [(protocol, sse_of(answer)) for protocol, answer in thinking.items()]
    # In Interactions, the summary may come whole in step.start instead.
    events = sse_of(thinking["interactions"])
    events[1][1]["step"]["summary"] = [events[2][1]["delta"]["content"]]
    runs.append(("interactions", events[:2] + events[3:]))
    for protocol, events in runs:
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
    ai.wait_list("upstream requests:", [
        "submitted  16", "completed  0", "cancelled  16", "rejected   0", "aborted    8",
        "preempted  0"])
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
# For the page wait0, the fake man exits at once, and its sleep still has
# the output pipe open.
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
big) head -c 5242880 /dev/zero | tr '\0' x ;;
endless) yes ;;
bytes) printf 'a\377b\303(c\342\202\254' ;;
slow) sleep 1; echo slow ;;
shadow) echo first ;;
shadowed) echo second ;;
wait*)
	sleep 1000 &
	echo "$$ $!" >> "$dir/waiting"
	[ "${page##*/}" = wait0 ] || wait
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
    and descriptors of its own; the page cut at 4 MiB and mended into UTF-8
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

            # A page is cut at 4 MiB, and made UTF-8 text.
            note = "\n\n[The page goes on past the 4 MiB shown.]\n"
            for _ in range(2):
                result = man_call(ai, name="big")
                assert not result.get("isError") and text_of(result) == \
                    "x" * (4 << 20) + note
            result = man_call(ai, name="endless")
            assert text_of(result) == "y\n" * (2 << 20) + note, len(text_of(result))
            assert text_of(man_call(ai, name="bytes")) == "a?b?(c€"
            assert runs("big") == (1, 1), runs("big")

            # One call waits out its deadline, and fifteen more go: sixteen
            # children at most, which go on until their deadline.
            # The man of wait0 exits at once, but its sleep has the output
            # pipe open.  So that man counts toward the limit of sixteen
            # children until askai kills its group at the deadline.
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
            wait_for(lambda: not alive(waiters()[0][0]))
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
        # The man of wait0 exits at once, and stopping askai kills its
        # group, which includes its sleep.
        os.unlink(f"{fake}/waiting")
        ai = Askai(binary, "-u", up.url(), "--man", env=env)
        with socket.create_connection(("127.0.0.1", ai.port), 10) as sock:
            sock.sendall(rpc_request(7, {"name": "man", "arguments": {"name": "wait0"}},
                                     "tools/call"))
            wait_for(lambda: len(waiters()) == 1)
            wait_for(lambda: not alive(waiters()[0][0]))
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
orphan)
    sleep 1000 &
    echo "$$ $!" > "$dir/pid.new" && mv "$dir/pid.new" "$dir/pid"
    echo begun ;;
*) exec /usr/bin/git "$@" ;;
esac
"""


def check_git(binary, up):
    """The git tool, with --git only: its arguments checked against the
    allowlists, revisions after --end-of-options and paths after --, run with
    a fixed environment and the settings that stop programs a repository
    could name; its output, errors included, made UTF-8 text and cut at
    4 MiB; at the deadline SIGTERM, then SIGKILL a second later; a call that
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
            f.write(b"x" * (5 << 20))
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
            # The description has an example call that reads a file with show
            # and HEAD:path, so that a model tries that first.
            assert '"revisions": ["HEAD:src/main.c"]' in tools[1]["description"] and \
                "HEAD:path" in schema["properties"]["revisions"]["description"], tools[1]
            assert text_of(call(ai, repo="proj", command="show",
                                revisions=["HEAD:a.txt"])).startswith("alpha\n"), "show"

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
                     "git cat-file takes no paths. To name a file, give the "
                     "revision HEAD:path."),
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
            assert not result.get("isError") and text_of(result) == "x" * (4 << 20) + \
                "\n\n[The output goes on past the 4 MiB shown.]", len(text_of(result))
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

            # This git exits at once, but its sleep has the output pipe open.
            # At the deadline, askai kills the group, which includes the
            # sleep.
            result = call(ai, repo="proj", command="log", revisions=["orphan"])
            assert result.get("isError") and text_of(result) == \
                "begun\n\n[git took longer than 1 s, and was stopped.]", result
            with open(f"{fake}/pid") as f:
                pids = [int(p) for p in f.read().split()]
            wait_for(lambda: not any(alive(p) for p in pids))

            # When the client closes the connection after this git has
            # exited, askai kills the group, which includes the sleep.
            os.unlink(f"{fake}/pid")
            with socket.create_connection(("127.0.0.1", ai.port), 10) as sock:
                sock.sendall(rpc_request(6, {"name": "git", "arguments": {
                    "repo": "proj", "command": "log", "revisions": ["orphan"]}},
                    "tools/call"))
                wait_for(lambda: os.path.exists(f"{fake}/pid"))
                with open(f"{fake}/pid") as f:
                    pids = [int(p) for p in f.read().split()]
                wait_for(lambda: not alive(pids[0]))
            wait_for(lambda: not alive(pids[1]))

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

        # A process can start askai with SIGCHLD ignored.  askai sets SIGCHLD
        # to its default action, so waitid() still returns the exit status
        # of git.
        with Askai(binary, "-u", up.url(), "--git", repo, env=env,
                   ignore=(signal.SIGCHLD,)) as ai:
            assert text_of(call(ai, repo="proj", command="log",
                                options=["--format=%s"])) == "first\n"

        # Stopping askai ends a call, killing its git; a configuration file
        # lists the repositories too.
        with open(f"{d}/askai.json", "w") as f:
            json.dump(grouped({"git": [repo, f"{d}/other"]}), f)
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


def turn_stream(calls=(), text="", reasoning="", used=None, order=(),
                index=True, reason=None, extras=None):
    """Return a streamed chat completion that contains the reasoning
    @p reasoning, then the text @p text, and then the tool calls in
    @p calls.  Each call is a tuple (id, name, JSON text of the
    arguments), and the arguments come in pieces of 7 bytes.  @p used is a
    tuple (prompt tokens, answer tokens) for the usage.  @p order is a
    list of call numbers.  For each number in @p order, the stream
    contains the next piece of that call.  Then the remaining pieces of
    each call follow, call by call.  With @p index false, the pieces have
    no index.  @p reason is the stop reason.  @p extras is a list with one
    item for each call: its extra_content, or None.  With @p extras, each
    call comes whole in one piece without an index, as Gemini sends
    calls."""
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
    rest = [iter(call) for call in pieces]
    ev += [next(rest[k]) for k in order]
    ev += [e for call in rest for e in call]
    ev.append((None, {"choices": [{"index": 0, "delta": {}, "finish_reason":
                                   reason or ("tool_calls" if calls else "stop")}]}))
    if used:
        ev.append((None, {"choices": [], "usage": {"prompt_tokens": used[0],
                                                   "completion_tokens": used[1]}}))
    ev.append((None, "[DONE]"))
    return Stream(wire=sse_wire(ev))


def messages_turn(blocks, stop="tool_use", stray=False, usage=None):
    """A streamed answer of Anthropic's Messages of the content @p blocks,
    each ("thinking", text, signature), ("redacted", data), ("text", text),
    ("tool", id, name, arguments' JSON text) or ("whole", id, name,
    arguments' JSON text), whose arguments come with its start, the texts
    and arguments in pieces of 5 bytes, that stops for @p stop; with
    @p stray, a text delta with no index after each block's start.  If
    @p usage is given, the message_start event contains it as the usage."""
    ev = [("message_start", {"type": "message_start", "message": {
        "id": "msg", "type": "message", "role": "assistant", "content": [],
        "usage": usage or {"input_tokens": 90, "cache_read_input_tokens": 10,
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


def interactions_turn(steps, status=None, usage=None, after=()):
    """A streamed answer of Gemini's Interactions of the output @p steps, each
    ("thought", texts of the summary items in its start, texts of those in
    deltas, signature or None), ("text", text in its start, text in deltas),
    ("call", id, name, arguments' JSON text in deltas) or ("whole", id,
    name, arguments' JSON text in its start), numbered in their order; the
    texts in deltas in pieces of 5 bytes and the arguments in pieces of 7.
    A dict in @p steps is the data of an event that is sent as it is.  The
    end has @p status, by default requires_action if a step is a call, else
    completed, and @p usage; the events of the dicts in @p after follow it."""
    def event(data):
        return (data["event_type"], data)

    def text(t):
        return {"type": "text", "text": t}

    ev = [event({"event_type": "interaction.created",
                 "interaction": {"id": "v1_x", "status": "in_progress"}})]
    calls = False
    i = 0
    for s in steps:
        if isinstance(s, dict):
            ev.append(event(s))
            continue
        kind = s[0]
        if kind == "thought":
            start = {"type": "thought", "signature": ""}
            start |= {"summary": [text(t) for t in s[1]]} if s[1] else {}
            deltas = [{"type": "thought_summary", "content": text(t)} for t in s[2]]
            deltas += [{"type": "thought_signature", "signature": s[3]}] if s[3] else []
        elif kind == "text":
            start = {"type": "model_output"}
            start |= {"content": [text(s[1])]} if s[1] else {}
            deltas = [{"type": "text", "text": t} for t in split(s[2], 5) if t]
        else:
            calls = True
            whole = kind == "whole"
            start = {"type": "function_call", "id": s[1], "name": s[2],
                     "arguments": json.loads(s[3]) if whole else {}}
            deltas = [] if whole else [{"type": "arguments_delta", "arguments": t}
                                       for t in split(s[3], 7)]
        ev.append(event({"event_type": "step.start", "index": i, "step": start}))
        ev += [event({"event_type": "step.delta", "index": i, "delta": d})
               for d in deltas]
        ev.append(event({"event_type": "step.stop", "index": i}))
        i += 1
    ev.append(event({"event_type": "interaction.completed", "interaction": {
        "id": "v1_x", "status": status or ("requires_action" if calls else "completed"),
        "usage": usage or {"total_input_tokens": 100, "total_output_tokens": 20,
                           "total_thought_tokens": 30, "total_tokens": 150}}}))
    ev += [event(data) for data in after]
    ev.append(("done", "[DONE]"))
    return Stream(wire=sse_wire(ev))


def check_notes(binary, up):
    """With --notes and without --upstream-tools, a role can grant the notes
    tool to clients.  An add without a thread starts one, in a repository of
    its own under the client's identity, and the answer names it.  A list
    has the notes of a thread, the oldest first, and a read returns the text
    of a note from the byte that offset names.  A note is never changed, and
    the names that git refuses are refused.  Of two adds at once, one can
    fail, and its answer asks to add the note again.  A dropped add still
    adds its note, and a stale lock file of git is removed.  A client reads
    the upstream's notebook of a thread and adds only to its own.  A thread
    of another identity is unknown to a client."""
    def digest(token):
        return "sha256:" + hashlib.sha256(token.encode()).hexdigest()

    def auth(token):
        return {"Authorization": f"Bearer {token}"}

    with tempfile.TemporaryDirectory() as d:
        notes = f"{d}/notes"
        path = f"{d}/askai.json"
        with open(path, "w") as f:
            json.dump(grouped({"url": up.url(), "notes": notes,
                               "roles": {"writer": {"tools": ["notes"]},
                                         "asker": {"tools": ["ask_ai"]}},
                               "identities": {
                                   "alice": {"role": "writer", "tokens": [digest("alice-1")]},
                                   "bob": {"role": "writer", "tokens": [digest("bob-1")]},
                                   "carol": {"role": "asker", "tokens": [digest("carol-1")]}}}),
                      f)
        os.chmod(path, 0o600)
        with Askai(binary, "-c", path) as ai:
            def call(arguments, token="alice-1"):
                status, _, body = ai.rpc("tools/call", {"name": "notes",
                                                        "arguments": arguments},
                                         headers=auth(token))
                assert status == 200, (status, body)
                if "error" in body:
                    return body["error"]["message"]
                result = body["result"]
                assert len(result["content"]) == 1, result
                return ("!" if result.get("isError") else "") + text_of(result)

            for token, tools in (("alice-1", ["notes"]), ("carol-1", ["ask_ai"])):
                listed = ai.rpc("tools/list", headers=auth(token))[2]["result"]["tools"]
                assert [t["name"] for t in listed] == tools, listed
            assert call({"action": "list"}, "carol-1") == "Tool not available: notes"

            said = call({"action": "add", "file": "//plan.md", "text": "step 1"})
            thread = re.fullmatch(r"Noted, in the new thread ([0-9a-f]{32})\. Give "
                                  r"this thread to add more notes to it, to list "
                                  r"them, or to read one\.", said).group(1)
            [who] = os.listdir(notes)
            assert re.fullmatch("[0-9a-f]{16}", who) and \
                os.listdir(f"{notes}/{who}") == ["client"], who
            assert os.listdir(f"{notes}/{who}/client") == [thread]
            long = "".join(f"line {i:04}\n" for i in range(800))
            assert call({"action": "add", "file": "a/b c.md", "text": long,
                         "thread": thread}) == "Noted."
            assert call({"action": "add", "file": 'q"\\.txt', "text": "q",
                         "thread": thread}) == "Noted."
            for name, why in (("plan.md", "A note of that name exists"),
                              ("a", "That name is a directory of notes"),
                              ("plan.md/x", "A directory in that name is a note"),
                              ("", "The file argument must be"),
                              ("/", "The file argument must be"),
                              ("a//b", "The file argument must be"),
                              ("a/", "The file argument must be"),
                              (".", "The file argument must be"),
                              ("x/../plan.md", "The file argument must be"),
                              ("x/.Git/y", "The file argument must be"),
                              (".git.", "The file argument must be"),
                              (".GIT ", "The file argument must be"),
                              ("git~1", "The file argument must be"),
                              ("x/GIT~1/y", "The file argument must be"),
                              (".git:x", "The file argument must be"),
                              ("a\\.git", "The file argument must be"),
                              ("a\tb", "The file argument must be"),
                              ("x" * 256, "The file argument must be")):
                said = call({"action": "add", "file": name, "text": "t",
                             "thread": thread})
                assert said.startswith("!" + why), (name, said)
            for text in ("", "x" * ((4 << 20) + 1), "a\0b"):
                assert call({"action": "add", "file": "t.md", "text": text,
                             "thread": thread}).startswith("!The text argument"), text
            assert call({"action": "add", "file": "x" * 255, "text": "t",
                         "thread": thread}) == "Noted."

            listed = call({"action": "list", "thread": thread})
            when = r"\d{4}-\d\d-\d\d \d\d:\d\d UTC"
            assert re.fullmatch(
                f"Your notes, oldest first:\n\n1\\. {when}  plan\\.md  \\(1 line\\)\n"
                f"2\\. {when}  a/b c\\.md  \\(800 lines\\)\n"
                f'3\\. {when}  q"\\\\\\.txt  \\(1 line\\)\n4\\. {when}  x{{255}}  '
                "\\(1 line\\)\n", listed), listed
            assert call({"action": "read", "file": "/plan.md", "thread": thread}) == \
                "step 1"
            assert call({"action": "read", "file": 'q"\\.txt', "thread": thread}) == "q"
            assert call({"action": "read", "file": "a/b c.md", "thread": thread,
                         "offset": 7990}) == long[7990:]
            assert call({"action": "read", "file": "a", "thread": thread}) == \
                "!That name is a directory of notes. list shows the notes in it."
            assert call({"action": "read", "file": "b", "thread": thread}) == \
                "!No note has that name."
            assert call({"action": "read", "file": "b", "thread": thread,
                         "offset": -1}).startswith("!The offset argument")
            # Only list and read take an offset, and the answer of a call
            # that failed is whole.
            said = call({"action": "add", "file": "o.md", "text": "o", "offset": 500})
            assert said.startswith("Noted, in the new thread "), said
            assert call({"action": "read", "file": "b", "thread": thread,
                         "offset": 5}) == "!No note has that name."

            for arguments, why in (
                    ({"action": "list"}, "list, read and copy need the thread argument"),
                    ({"action": "read", "file": "plan.md"},
                     "list, read and copy need the thread argument"),
                    ({"action": "copy", "file": "plan.md"},
                     "list, read and copy need the thread argument"),
                    ({"action": "list", "thread": "xyz"},
                     "The thread argument must be the id of a thread"),
                    ({"action": "remove", "thread": thread},
                     "The action argument must be add, list, read or copy.")):
                assert call(arguments).startswith("!" + why), (arguments, call(arguments))
            # A thread has a notebook of each side under one id.  A client
            # reads the upstream's notebook of a thread with "notebook":
            # "other", and adds notes only to its own: its first add to a
            # thread of the upstream starts its own notebook there.
            git_env = os.environ | {"GIT_CONFIG_GLOBAL": "/dev/null",
                                    "GIT_CONFIG_NOSYSTEM": "1"}
            shared = "1" * 32
            book = f"{notes}/{who}/upstream/{shared}"
            subprocess.run(["git", "init", "-q", "--bare", book], check=True, env=git_env)
            now = int(time.time())
            subprocess.run(["git", "--git-dir", book, "fast-import", "--quiet"],
                           input=(f"commit refs/heads/notes\ncommitter askai <askai> "
                                  f"{now} +0000\ndata 0\nM 100644 inline found.md\n"
                                  "data 6\nfound\n\n").encode(), check=True, env=git_env)
            stamp = time.strftime("%Y-%m-%d %H:%M UTC", time.gmtime(now))
            assert call({"action": "list", "thread": shared, "notebook": "other"}) == \
                f"The other AI's notes, oldest first:\n\n1. {stamp}  found.md  (1 line)\n"
            assert call({"action": "read", "file": "found.md", "thread": shared,
                         "notebook": "other"}) == "found\n"
            assert call({"action": "list", "thread": shared}) == \
                "!You have added no notes to this thread yet."
            for arguments, why in (
                    ({"action": "add", "file": "z.md", "text": "z", "notebook": "other"},
                     "!You can add notes only to your own notebook. The other "
                     "notebook can only be read."),
                    ({"action": "list", "notebook": "theirs"},
                     "!The notebook argument must be own or other.")):
                assert call(arguments | {"thread": shared}) == why, (arguments, why)
            assert call({"action": "add", "file": "mine.md", "text": "m",
                         "thread": shared}) == "Noted."
            assert shared in os.listdir(f"{notes}/{who}/client")
            assert "mine.md" in call({"action": "list", "thread": shared})
            assert "mine.md" not in call({"action": "list", "thread": shared,
                                          "notebook": "other"})
            assert call({"action": "list", "thread": thread, "notebook": "other"}) == \
                "!The other AI keeps no notes in this thread."
            # copy puts a note of the other notebook into the client's, byte
            # for byte, without its text in a call: also a file that is not
            # text, which list marks and read does not show.
            raw = bytes(range(256)) * 3
            subprocess.run(["git", "--git-dir", book, "fast-import", "--quiet"],
                           input=(f"commit refs/heads/notes\ncommitter askai <askai> "
                                  f"{now} +0000\ndata 0\nfrom refs/heads/notes^0\n"
                                  f"M 100644 inline img/raw.bin\ndata {len(raw)}\n"
                                  ).encode() + raw + b"\n", check=True, env=git_env)
            assert call({"action": "list", "thread": shared, "notebook": "other"}).endswith(
                "img/raw.bin  (binary)\n")
            for arguments, said in (
                    ({"file": "found.md"}, "Copied into your notebook."),
                    ({"file": "found.md", "as": "copies/f.md"}, "Copied into your notebook."),
                    ({"file": "img/raw.bin"}, "Copied into your notebook."),
                    ({"file": "found.md"}, "!A note of that name exists, and a note "
                     "cannot be changed. Add the note under another name."),
                    ({"file": "none.md"}, "!No note has that name."),
                    ({"file": "found.md", "as": "a//b"}, "!The as argument must be a "
                     "file name, as the file argument is."),
                    ({"file": "../x"}, "!The file argument must be")):
                got = call({"action": "copy", "thread": shared} | arguments)
                assert got.startswith(said), (arguments, got)
            for name in ("found.md", "copies/f.md"):
                assert call({"action": "read", "file": name, "thread": shared}) == "found\n"
            assert call({"action": "read", "file": "img/raw.bin", "thread": shared}) == \
                f"That note is not UTF-8 text: {len(raw)} bytes. It cannot be shown here."
            mine = f"{notes}/{who}/client/{shared}"
            assert subprocess.run(["git", "--git-dir", mine, "cat-file", "blob",
                                   "notes:img/raw.bin"], capture_output=True, check=True,
                                  env=git_env).stdout == raw
            # A client cannot use a thread of another identity, nor read its
            # other notebook.
            for arguments in ({"action": "list"}, {"action": "read", "file": "plan.md"},
                              {"action": "add", "file": "z.md", "text": "z"}):
                said = call(arguments | {"thread": thread}, "bob-1")
                assert said == f"!Unknown thread: {thread}.", (arguments, said)
            assert call({"action": "list", "thread": shared, "notebook": "other"},
                        "bob-1") == "!The other AI keeps no notes in this thread."
        # When two adds to one thread run at once, one of them fails, and its
        # answer asks to add the note again.  A git that sleeps before
        # fast-import makes both adds check the same newest note.
        os.mkdir(f"{d}/bin")
        with open(f"{d}/bin/git", "w") as f:
            f.write('#!/bin/sh\ncase " $* " in *" fast-import "*) sleep 0.5;; esac\n'
                    f'exec {shutil.which("git")} "$@"\n')
        os.chmod(f"{d}/bin/git", 0o755)
        with Askai(binary, "-c", path,
                   env={"PATH": f"{d}/bin:{os.environ['PATH']}"}) as ai:
            for k in range(3):
                said = []
                adds = [threading.Thread(target=lambda name=name: said.append(call(
                    {"action": "add", "file": name, "text": "r", "thread": thread})))
                    for name in (f"race/{k}a.md", f"race/{k}b.md")]
                for add in adds:
                    add.start()
                for add in adds:
                    add.join(10)
                assert sorted(said) == ["!Could not add the note, because another "
                                        "call added a note to this thread at the "
                                        "same time. Add it again.", "Noted."], said
            assert not any("could not add" in line for line in ai.log), ai.log
            # An add whose client goes while git writes still adds the note.
            sock = socket.create_connection(("127.0.0.1", ai.port), 10)
            sock.sendall(rpc_request(1, {"name": "notes", "arguments": {
                "action": "add", "file": "dropped.md", "text": "d",
                "thread": thread}}, "tools/call", headers=auth("alice-1")))
            time.sleep(0.2)
            sock.close()
            time.sleep(1)
            assert "dropped.md" in call({"action": "list", "thread": thread})
            # A lock file that git left behind is removed once it is a minute
            # old, and the next add works.
            book = f"{notes}/{who}/client/{thread}"
            for lock in ("HEAD.lock", "refs/heads/notes.lock"):
                with open(f"{book}/{lock}", "w"):
                    pass
                os.utime(f"{book}/{lock}", (time.time() - 120,) * 2)
            assert call({"action": "add", "file": "unlocked.md", "text": "u",
                         "thread": thread}) == "Noted."
            assert not os.path.exists(f"{book}/refs/heads/notes.lock")
        # A process can start askai with SIGCHLD ignored.  askai sets SIGCHLD
        # to its default action, so waitid() still returns the exit status
        # of git, and the note is added.
        with Askai(binary, "-c", path, ignore=(signal.SIGCHLD,)) as ai:
            said = call({"action": "add", "file": "y.md", "text": "y"})
            assert said.startswith("Noted, in the new thread "), said
    print("notes: granted by a role, threads of the clients' own, list, read, "
          "offsets, names and texts refused, races, drops, stale locks, other "
          "threads unknown: PASS")


def check_turn_apis(binary, up):
    """With --upstream-tools, the turns of the Responses API, Anthropic's
    Messages, Gemini's generateContent and Gemini's Interactions: the tools
    are offered as each has them, a turn goes back as it came, its items in
    their order, its thinking and its signatures whole, and its calls'
    results as each takes them; an answer that the turns doze after goes
    back as it came, then the message that wakes them, as the user's.  The
    replay of Interactions stays valid JSON when the events come in broken
    orders, and the token counts of its usage are checked against
    --context.  In chat completions, in Messages and in Interactions, askai
    treats token counts whose sum overflows int64_t as not known."""
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

        # In Gemini's Interactions, the model's steps go back in their order,
        # each in the form of the non-streamed answer.  A thought has its
        # summary items and its signature, and no empty signature.  A model
        # output has the text of its start and of its deltas, and a step of
        # blank text is left out.  A call has the arguments of its deltas or
        # of its start.  Each result is a step after the model's steps.
        def text(t):
            return {"type": "text", "text": t}

        def output(t):
            return {"type": "model_output", "content": [text(t)]}

        def fcall(cid, name, args):
            return {"type": "function_call", "id": cid, "name": name, "arguments": args}

        def thought(summary, signature=None):
            return {"type": "thought"} | ({"summary": [text(t) for t in summary]}
                                          if summary else {}) | \
                ({"signature": signature} if signature else {})

        def event(kind, at=None, **data):
            return {"event_type": "step." + kind} | \
                ({"index": at} if at is not None else {}) | data

        broken = '{"repo": "proj", "comm'
        first_turn = [("thought", ["Plan "], ["it.", "Then go."], "sig/+1"),
                      ("text", "Lo", "oking."), ("text", "", " \n\n"),
                      ("call", "c1", "git", json.dumps(log)),
                      ("call", "c2", "git", broken),
                      ("whole", "c3", "git", json.dumps(log))]
        # The events of the second turn come in broken orders.  They have a
        # second signature in a delta after one in the start, deltas of an
        # index that is not open or of a type that does not fit the step, a
        # summary item after the signature, a thought that is open until the
        # next step starts, a call or the arguments of a call without a start
        # while a thought is open, the arguments and the stop of a call that
        # started before the open thought, a step of a server tool and its
        # stop while a thought is open, text and a start without an index
        # while no step is open, summary items that are no object, arguments
        # that are no string, a thought that is open at the end, and steps
        # after the end, which are ignored.
        second_turn = [
            event("start", 0, step={"type": "thought", "signature": "first",
                                     "summary": [text("s1")]}),
            event("delta", 0, delta={"type": "thought_summary", "content": text("s2")}),
            event("delta", 0, delta={"type": "thought_signature", "signature": "second"}),
            event("delta", 7, delta={"type": "thought_summary", "content": text("x")}),
            event("delta", 7, delta={"type": "thought_signature", "signature": "y"}),
            event("delta", 7, delta={"type": "text", "text": "Z"}),
            event("delta", 0, delta={"type": "text", "text": "Z"}),
            event("stop", 0),
            event("start", 1, step={"type": "thought", "signature": ""}),
            event("delta", 1, delta={"type": "thought_summary", "content": text("s3")}),
            event("delta", 7, delta={"type": "thought_signature", "signature": "y"}),
            event("delta", 1, delta={"type": "thought_signature", "signature": "sig2"}),
            event("delta", 1, delta={"type": "thought_summary", "content": text("s4")}),
            event("start", 2, step={"type": "model_output", "content": [text("Hi")]}),
            event("delta", delta={"type": "text", "text": "X"}),
            event("delta", 2, delta={"type": "text", "text": " there"}),
            event("stop", 2),
            event("delta", delta={"type": "text", "text": "Y"}),
            event("start", step={"type": "thought", "summary": [text("q")]}),
            event("start", 3, step={"type": "function_call", "id": "c4", "name": "git",
                                    "arguments": {}}),
            event("start", 4, step={"type": "thought", "summary": [text("s5")]}),
            event("delta", 3, delta={"type": "arguments_delta",
                                     "arguments": json.dumps(log)}),
            event("stop", 3),
            event("delta", 4, delta={"type": "thought_summary"}),
            event("delta", 4, delta={"type": "thought_summary", "content": "s"}),
            event("start", 5, step={"type": "google_search_call", "id": "g1"}),
            event("delta", 5, delta={"type": "google_search_call", "arguments": {}}),
            event("stop", 5),
            event("delta", 4, delta={"type": "thought_summary", "content": text("s6")}),
            event("start", 6, step={"type": "function_call", "id": "c5", "name": "git",
                                    "arguments": log}),
            event("stop", 6),
            event("start", 7, step={"type": "model_output", "content": [
                text("Ok"), {"type": "text_annotation", "text": "no"}]}),
            event("delta", 7, delta={"type": "thought_summary", "content": text("x")}),
            event("delta", 7, delta={"type": "thought_signature", "signature": "y"}),
            event("delta", 7, delta={"type": "text", "text": "."}),
            event("stop", 7),
            event("start", 8, step={"type": "thought", "summary": [text("s7")]}),
            event("delta", 9, delta={"type": "arguments_delta",
                                     "arguments": json.dumps(log)}),
            event("start", 10, step={"type": "thought", "summary": [text("s8")]}),
            event("delta", 11, delta={"type": "arguments_delta", "arguments": {"a": 1}}),
            event("delta", delta={"type": "text", "text": "!"})]
        with Askai(binary, "--api", "interactions", "-u", up.url("/v1beta/interactions"),
                   "-m", "m", "-s", "SYS", "-n", "100", "--upstream-tools",
                   "--git", repo) as ai:
            late = [event("start", 20, step={"type": "thought", "summary": [text("late")]}),
                    event("start", 21, step={"type": "function_call", "id": "c9",
                                            "name": "git", "arguments": log})]
            script[:] = [lambda r: (200, interactions_turn(first_turn)),
                         lambda r: (200, interactions_turn(second_turn, after=late)),
                         lambda r: (200, interactions_turn([
                             ("text", "A", ""),
                             event("delta", delta={"type": "text", "text": "B"})]))]
            count = len(up.requests)
            result = ai.ask("Q")
            assert result["content"][0]["text"] == "AB", result
            first, second, third = up.requests[count:]
            body = first["body"]
            assert [(k, body[k]) for k in ("model", "system_instruction", "store",
                                           "stream", "generation_config")] == \
                [("model", "m"), ("system_instruction", "SYS"), ("store", False),
                 ("stream", True), ("generation_config", {"max_output_tokens": 100})], \
                body
            tools = body["tools"]
            assert [(t["type"], t["name"]) for t in tools] == \
                [("function", t) for t in TURN_TOOLS], tools
            assert all(sorted(t) == ["description", "name", "parameters", "type"]
                       for t in tools), tools
            assert tools[0]["parameters"]["properties"]["repo"], tools
            given = second["body"]["input"]
            assert list(second["body"])[-1] == "input", second["body"]
            user = {"type": "user_input", "content": [text("Q")]}
            assert given[:6] == [
                user, thought(["Plan ", "it.", "Then go."], "sig/+1"), output("Looking."),
                fcall("c1", "git", log), fcall("c2", "git", {}),
                fcall("c3", "git", log)], given
            assert b'"signature":"sig/+1"' in second["raw"], second["raw"]
            assert [(g["type"], g["call_id"]) for g in given[6:]] == \
                [("function_result", c) for c in ("c1", "c2", "c3")], given
            assert re.fullmatch(logged, given[6]["result"]), given
            assert given[7]["result"] == "Your arguments were not a JSON object, " \
                "so the call was not run.", given
            assert re.fullmatch(logged, given[8]["result"]), given
            assert [sorted(g) for g in given[6:]] == [["call_id", "result", "type"]] * 3, \
                given
            given = third["body"]["input"]
            assert given[:9] == second["body"]["input"], given
            made = given[17].get("id", "")
            assert re.fullmatch(r"call_\d+_\d+", made), given
            assert given[9:19] == [
                {"type": "thought", "signature": "first", "summary": [text("s1"),
                                                                      text("s2")]},
                thought(["s3"], "sig2"), output("Hi there"), fcall("c4", "git", log),
                thought(["s5", "s6"]), fcall("c5", "git", log), output("Ok."),
                thought(["s7"]), fcall(made, "", log), thought(["s8"])], given
            assert [(g["type"], g["call_id"]) for g in given[19:]] == \
                [("function_result", c) for c in ("c4", "c5", made)], given

            # An answer that the turns doze after, a thought and text, goes
            # back as it came, then the message that wakes them.
            given = doze(ai, [interactions_turn([("whole", "t1", "timer",
                                                  '{"seconds":1}')]),
                              interactions_turn([("thought", [], [], "sigW"),
                                                 ("text", "Wait", "ing.")]),
                              interactions_turn([("text", "A", "")])], "input")
            assert given[3:] == [thought([], "sigW"), output("Waiting."),
                                 {"type": "user_input", "content": [text(woke)]}], given

        # The tokens used are total_input_tokens plus total_output_tokens,
        # which leave out the thinking.  If that sum is not known, the tokens
        # used are total_tokens.  An input count that is not a JSON integer,
        # such as NaN, which json-c parses, is not known.  When the tokens
        # used fill the context, askai starts the conversation again in the
        # next request.
        for usage, full in (({"total_input_tokens": 7990, "total_output_tokens": 10,
                              "total_thought_tokens": 500, "total_tokens": 8500}, True),
                            ({"total_input_tokens": 7990.0, "total_output_tokens": 10,
                              "total_tokens": 100}, False),
                            ({"total_tokens": 8000}, True),
                            ({"total_input_tokens": float("nan"),
                              "total_output_tokens": 10, "total_tokens": 8000}, True),
                            ({"total_input_tokens": 100, "total_output_tokens": 20,
                              "total_thought_tokens": 7880, "total_tokens": 8000},
                             False)):
            with Askai(binary, "--api", "interactions",
                       "-u", up.url("/v1beta/interactions"), "--upstream-tools",
                       "--git", repo, "--context", "8000") as ai:
                script[:] = [lambda r, u=usage: (200, interactions_turn(
                                 [("call", "c1", "git", json.dumps(log))], usage=u)),
                             lambda r: (200, interactions_turn([("text", "A", "")]))]
                count = len(up.requests)
                result = ai.ask("Q")
                assert result["content"][0]["text"] == "A", (usage, result)
                given = up.requests[count + 1]["body"]["input"]
                said = given[0]["content"][0]["text"]
                assert (len(given) == 1 and said.startswith(
                    "Q\n\n[askai: your context filled up")) == full, (usage, given)

        # askai treats token counts whose sum overflows int64_t as not known.
        # A count that is not known does not fill the context, so the second
        # request continues the conversation.  Without the overflow check,
        # these sums wrap to a count near UINT32_MAX, and the second request
        # starts the conversation again.
        big = 2**63 - 1
        call = [("a", "git", json.dumps(log))]
        for api, path, first, second in (
                ("chat", "/v1/chat/completions", turn_stream(call, used=(big, big)),
                 turn_stream(text="A")),
                ("messages", "/v1/messages", messages_turn(
                    [("tool", *call[0])], usage={
                        "input_tokens": big, "cache_creation_input_tokens": big,
                        "cache_read_input_tokens": big}),
                 messages_turn([("text", "A")], "end_turn")),
                ("interactions", "/v1beta/interactions", interactions_turn(
                    [("call", *call[0])], usage={"total_input_tokens": big,
                                                 "total_output_tokens": big}),
                 interactions_turn([("text", "A", "")]))):
            with Askai(binary, "--api", api, "-u", up.url(path), "--upstream-tools",
                       "--git", repo, "--context", "8000") as ai:
                script[:] = [lambda r, a=first: (200, a), lambda r, a=second: (200, a)]
                count = len(up.requests)
                result = ai.ask("Q")
                assert result["content"][0]["text"] == "A", (api, result)
                body = json.dumps(up.requests[count + 1]["body"])
                assert "your context filled up" not in body, (api, body)
    up.reply = lambda request: (200, ANSWERS["openai"])
    print("turns of the Responses API, Anthropic's Messages, Gemini's "
          "generateContent and Interactions: tools, items in order, thinking and "
          "signatures given back, results, waking: PASS")


def check_turns(binary, up):
    """With --upstream-tools, an ask call lends the upstream the tools that
    its client's role lets the upstream call, never itself, and notes and
    compact, in chat
    completions: it runs the tools each turn calls, a tool's output cut to a
    window that offset moves, writes the turn and the results into the
    conversation, and asks on until an answer calls none, --max-turns turns
    at most; the client gets the answer and the calls' trace.  Notes are
    files in the repository of the thread, listed and read back; their list
    and the newest note go with the prompt to a call that goes on with the
    thread.  compact has the conversation start again from the prompt, the
    summary and the notes, and a context that is full, as the upstream
    reports or as askai counts, has it start again from the prompt and the
    notes."""
    def run(*args):
        return subprocess.run([binary, "-u", up.url(), *args], capture_output=True,
                              text=True, timeout=10)

    for args, message in ((("--upstream-tools", "--api", "generate", "-x",
                            '{"contents": null}'), "--extra may not change contents "
                           "with --upstream-tools"),
                          (("--upstream-tools", "--api", "interactions", "-x",
                            '{"input": null}'), "--extra may not change input "
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
        sh("init", "-q", "--bare", f"{d}/old", cwd=d)
        for path, message in ((f"{d}/file", "not a directory"),
                              (f"{d}/old", "the directory is a git repository")):
            proc = run("--upstream-tools", "--notes", path)
            assert proc.returncode and message in proc.stderr, (path, proc)
        # A time in a list of notes.
        when = r"\d{4}-\d\d-\d\d \d\d:\d\d UTC"

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
            assert "thread" in tools["notes"]["inputSchema"]["properties"], tools
            assert "compact" not in tools, tools

            # Two calls, one of git and one of notes, then a list of the notes,
            # then the answer.  The turns carry the text, the thinking and the
            # calls.
            log = json.dumps({"repo": "proj", "command": "log",
                              "options": ["--oneline"]})
            script[:] = [
                lambda r: (200, turn_stream([("c1", "git", log), ("c2", "notes", json.dumps(
                    {"action": "add", "file": "/found.md", "text": "found \"first\""}))],
                    text="Looking.", reasoning="hmm", used=(100, 20))),
                lambda r: (200, turn_stream([("", "notes", '{"action":"list"}')])),
            ]
            count = len(up.requests)
            result = ai.ask("What is there?")
            first, second, third = up.requests[count:]
            body = first["body"]
            assert [t["function"]["name"] for t in body["tools"]] == \
                ["git", "notes", "compact", "background", "jobs", "wait", "timer"], \
                body["tools"]
            # The upstream's notes use the thread of the ask call.
            assert "thread" not in body["tools"][1]["function"]["parameters"][
                "properties"], body["tools"][1]
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
            listed = read[1]["content"]
            assert read[1]["tool_call_id"] == "call_2_0" and re.fullmatch(
                f"Your notes, oldest first:\n\n1\\. {when}  found\\.md  \\(1 line\\)\n",
                listed), read
            text, trace = [c["text"] for c in result["content"]]
            assert text == "A", result
            thread = re.search(r"in the thread ([0-9a-f]{32}):", trace).group(1)
            assert trace.startswith("The other AI made 3 tool calls in 3 turns:\n"
                                    f"- git {log}: 14 bytes\n"
                                    "- notes add: 6 bytes\n"
                                    f"- notes list: {len(listed)} bytes\n"), trace
            assert "found" not in trace, trace
            book = f"{notes}/anonymous/upstream/{thread}"
            assert sh("--git-dir", book, "show", "notes:found.md",
                      cwd=d) == "found \"first\"", book

            # When an ask call goes on with a thread, the prompt has the list
            # of the notes of the thread and the newest note.
            count = len(up.requests)
            result = ai.ask("And now?", thread=thread)
            assert re.fullmatch(
                "And now\\?\n\n\\[askai: the notes you kept in earlier calls on "
                f"this thread, oldest first:\\]\n\n1\\. {when}  found\\.md  "
                "\\(1 line\\)\n\n\\[askai: your newest note, found\\.md:\\]\n\n"
                "found \"first\"\n", prompt(up.requests[count])), up.requests[count]
            assert f"in the thread {thread}:" in result["content"][1]["text"], result

            # Hooks in the repository of a thread do not run.  A note of a
            # name that a note has is refused.
            os.makedirs(f"{book}/hooks")
            with open(f"{book}/hooks/reference-transaction", "w") as f:
                f.write(f"#!/bin/sh\ntouch {d}/hooked\n")
            os.chmod(f"{book}/hooks/reference-transaction", 0o755)
            script[:] = [lambda r: (200, turn_stream([
                ("h", "notes", json.dumps({"action": "add", "file": "next.md",
                                           "text": "n"})),
                ("i", "notes", json.dumps({"action": "add", "file": "found.md",
                                           "text": "n"})),
                ("j", "notes", json.dumps({"action": "read", "file": "next.md"}))]))]
            count = len(up.requests)
            ai.ask("Q", thread=thread)
            assert [m["content"] for m in turns(up.requests[count + 1])[1:]] == [
                "Noted.", "A note of that name exists, and a note cannot be "
                "changed. Add the note under another name.", "n"], \
                up.requests[count + 1]
            assert not os.path.exists(f"{d}/hooked")
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
                    ("c", "notes", json.dumps({"action": "add", "file": "x.md",
                                               "text": "x" * ((4 << 20) + 1)})),
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
            assert outputs[2] == "The text argument must be a string of 1 byte to " \
                "4 MiB, and no NUL.", outputs[2]
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
            # output token limit cut; a name of a note that is not valid.
            script[:] = [
                lambda r: (200, turn_stream([("x", "git", log), ("y", "git", log)],
                                            index=False)),
                lambda r: (200, turn_stream([("z", "git", log[:-9])], reason="length")),
                lambda r: (200, turn_stream([("n", "notes", json.dumps(
                    {"action": "read", "file": "a/../b"}))])),
            ]
            count = len(up.requests)
            assert ai.ask("Q")["content"][0]["text"] == "A"
            outputs = [m["content"] for m in turns(up.requests[count + 1])[1:]]
            assert len(outputs) == 2 and outputs[0] == outputs[1], outputs
            assert turns(up.requests[count + 2])[-1]["content"] == "Your answer " \
                "reached the output token limit, which cut these arguments short, " \
                "so the call was not run.", up.requests[count + 2]
            assert turns(up.requests[count + 3])[-1]["content"].startswith(
                "The file argument must be a file name"), up.requests[count + 3]

            values = ai.metrics()
            assert total(values, "askai_turns_total") == 12, values
            assert [total(values, "askai_turn_tool_calls_total", tool=t) for t in
                    ("git", "notes", "man", "compact", "unknown")] == [8, 7, 0, 0, 1], \
                values

            # A long newest note goes into the prompt cut to the window of a
            # tool's output, with the offset to read on from.
            size = 100 << 10
            script[:] = [lambda r: (200, turn_stream([
                ("k", "notes", json.dumps({"action": "add", "file": "long.md",
                                           "text": "L" * size}))]))]
            ai.ask("Q", thread=thread)
            count = len(up.requests)
            ai.ask("Again?", thread=thread)
            shown = 64 << 10
            assert prompt(up.requests[count]).endswith(
                "[askai: your newest note, long.md:]\n\n" + "L" * shown +
                f"\n[askai: bytes 0 to {shown} of {size}; read the note with "
                f"\"offset\": {shown} for the rest.]\n"), prompt(up.requests[count])[-300:]

            # A thread that the client started with its own notes: an ask call
            # goes on with it, its turns read the client's notebook and start
            # their own, and the client reads that one.
            def client_notes(arguments):
                body = ai.rpc("tools/call", {"name": "notes",
                                             "arguments": arguments})[2]
                return text_of(body["result"])
            said = client_notes({"action": "add", "file": "brief.md", "text": "aim"})
            mine = re.search(r"new thread ([0-9a-f]{32})", said).group(1)
            script[:] = [lambda r: (200, turn_stream([
                ("m", "notes", json.dumps({"action": "list", "notebook": "other"})),
                ("n", "notes", json.dumps({"action": "read", "file": "brief.md",
                                           "notebook": "other"})),
                ("o", "notes", json.dumps({"action": "add", "file": "brief.md",
                                           "text": "seen", "notebook": "other"})),
                ("p", "notes", json.dumps({"action": "add", "file": "seen.md",
                                           "text": "seen"})),
                ("q", "notes", json.dumps({"action": "copy", "file": "brief.md"})),
                ("r", "notes", json.dumps({"action": "read", "file": "brief.md"}))]))]
            count = len(up.requests)
            result = ai.ask("Go on", thread=mine)
            assert prompt(up.requests[count]) == "Go on\n\n[askai: you have no notes " \
                "in this thread yet. The client keeps notes in it, which notes reads " \
                "with \"notebook\": \"other\".]\n", prompt(up.requests[count])
            outputs = [m["content"] for m in turns(up.requests[count + 1])[1:]]
            assert re.fullmatch(f"The client's notes, oldest first:\n\n1\\. {when}  "
                                "brief\\.md  \\(1 line\\)\n", outputs[0]), outputs
            assert outputs[1:] == ["aim", "You can add notes only to your own notebook. "
                                   "The other notebook can only be read.", "Noted.",
                                   "Copied into your notebook.", "aim"], outputs
            assert f"in the thread {mine}:" in result["content"][1]["text"], result
            assert "seen.md" in client_notes({"action": "list", "thread": mine,
                                              "notebook": "other"})
            count = len(up.requests)
            ai.ask("Again", thread=mine)
            assert prompt(up.requests[count]).startswith(
                "Again\n\n[askai: the client keeps notes in this thread too, which "
                "notes reads with \"notebook\": \"other\".]\n\n[askai: the notes you "
                "kept in earlier calls on this thread, oldest first:]"), \
                prompt(up.requests[count])

        # A small context: windows of an eighth of it, a reminder to write
        # notes past half, a warning past two thirds, and once full, the
        # conversation starts again.
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
            assert out.startswith("line 00000\n") and re.search(
                r"line 00362\n\n\n\[Bytes 0 to 3993 of 44000: for the rest, call "
                r"again with \"offset\": 3993\.\]\n\n\[askai: your context is "
                r"6[0-6]% full, \d+ of 8000 tokens\. Now is a good time to write "
                r"notes of what you have learned\. Compaction removes the "
                r"conversation, but not the notes\.\]$", out), out[-300:]
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

        # The reminders to write notes: one at a quarter of the context and
        # one at half, each only once.  From two thirds on, each turn's last
        # result asks for compact.  After a compaction, the reminders start
        # again.  Without the notes tool, there are no reminders.
        remind = (r"\n\n\[askai: your context is 2\d% full, \d+ of 8000 tokens\. "
                  r"Now is a good time to write notes of what you have learned\. "
                  r"Compaction removes the conversation, but not the notes\.\]")
        warn = (r"\n\n\[askai: your context is 6\d% full, \d+ of 8000 tokens\. "
                r"Note what you need to keep, then call compact\.\]")
        for notes_on in (True, False):
            # The last two arguments are --notes and its directory.
            what = args if notes_on else args[:-2]
            call = (lambda c: (c, "notes", json.dumps(
                        {"action": "add", "file": f"{c}.md", "text": c}))) \
                if notes_on else (lambda c: (c, "git", log))
            out = r"Noted\." if notes_on else r"[0-9a-f]{7} first\n"
            with Askai(binary, *what, "--context", "8000") as ai:
                # The tokens after each turn: 13%, 26%, 31%, 52%, 57%, 68%, 69%,
                # then compact, then 26% again.
                used = (1000, 2100, 2500, 4100, 4500, 5400, 5500)
                script[:] = [lambda r, c=c, u=u: (200, turn_stream([call(c)],
                                                                   used=(u, 10)))
                             for c, u in zip("abcdefg", used)] + [
                    lambda r: (200, turn_stream([("h", "compact", json.dumps(
                        {"summary": "S"}))], used=(5600, 10))),
                    lambda r: (200, turn_stream([call("i")], used=(2100, 10))),
                ]
                count = len(up.requests)
                assert ai.ask("Q")["content"][0]["text"] == "A"
                sent = up.requests[count + 1:]
                assert len(sent) == 9 and turns(sent[7]) == [], sent
                results = [turns(r)[-1]["content"] for r in sent[:7] + sent[8:]]
                half = remind.replace("2\\d%", "5\\d%")
                want = ["", remind, "", half, "", warn, warn, remind]
                if not notes_on:
                    want = [w if w == warn else "" for w in want]
                for got, notice in zip(results, want):
                    assert re.fullmatch(out + notice, got), (notes_on, results)

        with Askai(binary, *args, "--max-turns", "3") as ai:
            # compact: from the prompt, the summary and the notes; calls after
            # it do not run.
            script[:] = [
                lambda r: (200, turn_stream([("a", "notes", json.dumps(
                    {"action": "add", "file": "n1.md", "text": "n1"}))])),
                lambda r: (200, turn_stream([("b", "compact", json.dumps(
                    {"summary": "S"})), ("c", "git", log)])),
            ]
            count = len(up.requests)
            result = ai.ask("Q")
            again = up.requests[count + 2]
            assert turns(again) == [] and re.fullmatch(
                "Q\n\n\\[askai: you compacted the context: the conversation since "
                "the task is gone, your turns and the tools' results with it\\. "
                "Your summary of it:\\]\n\nS\n\n\\[askai: your notes, oldest "
                f"first:\\]\n\n1\\. {when}  n1\\.md  \\(1 line\\)\n\n\\[askai: your "
                "newest note, n1\\.md:\\]\n\nn1\n\n\\[askai: go on with the task\\.\\]",
                prompt(again)), prompt(again)
            assert "- compact: 1 bytes\n" in result["content"][1]["text"], result
            assert "git" not in result["content"][1]["text"], result

            # The upstream reports that the context is full: once, from the
            # prompt and the notes again; twice, the call fails.
            full = {"error": {"code": 400, "type": "exceed_context_size_error",
                              "message": "request (9000 tokens) exceeds the "
                              "available context size (8192 tokens), try "
                              "increasing it"}}
            script[:] = [lambda r: (200, turn_stream([("a", "notes", json.dumps(
                             {"action": "add", "file": "n2.md", "text": "n2"}))])),
                         lambda r: (400, full)]
            count = len(up.requests)
            assert ai.ask("Q")["content"][0]["text"] == "A"
            again = up.requests[count + 2]
            assert turns(again) == [] and re.fullmatch(
                "Q\n\n\\[askai: your context filled up, so the conversation since "
                "the task is gone, your turns and the tools' results with it\\.\\]"
                f"\n\n\\[askai: your notes, oldest first:\\]\n\n1\\. {when}  n2\\.md  "
                "\\(1 line\\)\n\n\\[askai: your newest note, n2\\.md:\\]\n\nn2\n\n"
                "\\[askai: go on with the task\\.\\]", prompt(again)), prompt(again)
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

            # The pieces of parallel calls can come in any order.  The next
            # request lists the calls in the order in which they began.  In
            # the first stream, the pieces of the two calls alternate until
            # the pieces of call a end.  In the second stream, both calls
            # begin, then all arguments of call b come, and then all
            # arguments of call a.  In the third stream, call b begins before
            # call a.
            other = json.dumps({"repo": "proj", "command": "log",
                                "options": ["-1", "--format=%s"]})
            calls = [("a", "git", log), ("b", "git", other)]
            pieces = [1 + -(-len(args) // 7) for _, _, args in calls]
            assert pieces[0] < pieces[1], pieces
            for order, want in (([0, 1] * pieces[0], calls),
                                ([0] + [1] * pieces[1], calls),
                                ([1, 0], calls[::-1])):
                script[:] = [lambda r: (200, turn_stream(calls, order=order))]
                count = len(up.requests)
                result = ai.ask("Q")
                assert result["content"][0]["text"] == "A", (order, result)
                head, *results = turns(up.requests[count + 1])
                assert [(c["id"], c["function"]["name"], c["function"]["arguments"])
                        for c in head["tool_calls"]] == want, (order, head)
                assert [m["tool_call_id"] for m in results] == \
                    [cid for cid, _, _ in want], results

            # More than 16 calls in one turn are an error of the upstream.
            script[:] = [lambda r: (200, turn_stream([(str(i), "git", log)
                                                      for i in range(17)],
                                                     order=range(17)))]
            result = ai.ask("Q")
            assert result.get("isError") and result["content"][0]["text"] == \
                "Upstream error (HTTP 200): tool calls out of order, or too many", result

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
        with Askai(binary, *args, "--verbose",
                   env={"PATH": f"{d}/bin:" + os.environ["PATH"]}) as ai:
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
            # The list shows the job's arguments exactly as the turn sent
            # them.  The job has its own copy, which stays valid after askai
            # frees the arguments of the turn's call of background.
            wire = json.dumps(json.loads(slow), separators=(",", ":"))
            assert re.fullmatch(r"job 1: git " + re.escape(wire) + r": finished, \d+ bytes\n",
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
            # With --verbose, askai logs each call of a turn and each job that
            # answers or fails.  The line contains the ask call and the turn,
            # the name of the tool, whether the call failed, the time, the
            # size of the output, the arguments as compact JSON and the text
            # of a failure.
            ai.wait_log(r'^askai: ask call \d+\.\d+ turn \d+: tool call background '
                        r'answered after \d+\.\d s \(14 bytes\); arguments \{"tool":'
                        r'"git","arguments":\{"repo":"proj","command":"log","paths":'
                        r'\["SLOWPATH"\]\}\}$')
            for pattern in ("tool call background failed",
                            "tool call wait failed"):
                ai.wait_log(r"^askai: ask call \d+\.\d+ turn \d+: " + pattern +
                            r' after \d+\.\d s \(\d+ bytes\); arguments \{.*\}; '
                            r'error ".+"$')
            for pattern in ("tool call jobs answered",
                            "background tool call git answered"):
                ai.wait_log(r"^askai: ask call \d+\.\d+ turn \d+: " + pattern +
                            r" after \d+\.\d s \(\d+ bytes\); arguments \{.*\}$")

            # The output token limit can cut the arguments of a call short.
            # askai does not run such a call, and the time in the log line of
            # the call is under one second.  The line says why the arguments
            # are no object.
            script[:] = [lambda r: (200, turn_stream([("z", "git", slow[:-9])],
                                                     reason="length"))]
            assert ai.ask("Q")["content"][0]["text"] == "A"
            ai.wait_log(r"^askai: ask call \d+\.\d+ turn 1: tool call git failed after "
                        r"0\.\d s \(\d+ bytes\); arguments cut short by the output token "
                        r'limit; error "Your answer reached the output token limit, which '
                        r'cut these arguments short, so the call was not run\."$')

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

            # The list action returns every note, the oldest first.  The read
            # action returns a window of a long note, and a read with an offset
            # returns the rest.
            add = lambda c: json.dumps({"action": "add", "file": f"{c}/{c}.txt",
                                        "text": f"{c * 1199}\n" * 6})
            script[:] = [
                lambda r: (200, turn_stream([(c, "notes", add(c)) for c in "bca"])),
                lambda r: (200, turn_stream([
                    ("d", "notes", '{"action":"list"}'),
                    ("e", "notes", json.dumps({"action": "read", "file": "/c/c.txt"})),
                    ("o", "notes", json.dumps({"action": "read", "file": "c/c.txt",
                                               "offset": 6000})),
                    ("f", "notes", json.dumps({"action": "read", "file": "c"})),
                    ("g", "notes", json.dumps({"action": "read", "file": "z"}))])),
            ]
            count = len(up.requests)
            ai.ask("Q")
            reads = [m["content"] for m in turns(up.requests[count + 2])[5:]]
            assert re.fullmatch("Your notes, oldest first:\n\n" + "".join(
                f"{k}\\. {when}  {c}/{c}\\.txt  \\(6 lines\\)\n"
                for k, c in ((1, "b"), (2, "c"), (3, "a"))), reads[0]), reads[0]
            assert reads[1] == f"{'c' * 1199}\n" * 2 + "\n\n[Bytes 0 to 2400 of 7200: " \
                "for the rest, call again with \"offset\": 2400.]", reads[1][-100:]
            assert reads[2] == f"{'c' * 1199}\n\n\n[Bytes 6000 to 7200 of 7200: its " \
                "end.]", reads[2][-100:]
            # The result of the last call also has the warning that the context
            # is full.
            assert reads[3] == "That name is a directory of notes. list shows the " \
                "notes in it." and re.fullmatch(
                    r"No note has that name\.\n\n\[askai: your context is \d+% full, "
                    r"\d+ of 6000 tokens\. Note what you need to keep, then call "
                    r"compact\.\]", reads[4]), reads[3:]

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

        # The member "upstream" of a role lists the tools that the upstream
        # may call.  This list is separate from the list of tools that the
        # role's clients may call.
        for role, listed, lent in (
                ({"tools": ["ask_ai"], "upstream": ["git"]}, ["ask_ai"],
                 ["git", "notes", "compact", "background", "jobs", "wait", "timer"]),
                ({"tools": ["ask_ai", "git"], "upstream": []}, ["ask_ai", "git"],
                 ["notes", "compact", "wait", "timer"])):
            with open(path, "w") as f:
                json.dump({"roles": {"asker": role},
                           "anonymous": {"loopback": "asker"}}, f)
            with Askai(binary, *args, "-c", path) as ai:
                tools = ai.rpc("tools/list")[2]["result"]["tools"]
                assert [t["name"] for t in tools] == listed, tools
                count = len(up.requests)
                assert text_of(ai.ask("Q")) == "A"
                body = up.requests[count]["body"]
                assert [t["function"]["name"] for t in body["tools"]] == lent, body
        # askai does not offer the tool ask_ai to the upstream.
        with open(path, "w") as f:
            json.dump({"roles": {"r": {"tools": ["ask_ai"], "upstream": ["ask_ai"]}}}, f)
        run = subprocess.run([binary, *args, "-c", path], capture_output=True,
                             text=True, timeout=5)
        assert run.returncode == 2 and \
            'role r: askai offers no tool "ask_ai" to the upstream' in run.stderr, \
            run.stderr
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
    wait_for(lambda: len(ai.listed("tasks:") or ()) == 6)
    counts = {name: int(n) for name, n in
              (item.rsplit(None, 1) for item in ai.listed("tasks:"))}
    assert counts["submitted"] == counts["done"] + counts["cancelled"] + \
        counts["abandoned"] == 3 + 40 * rounds and counts["moved"] == moves and \
        counts["under way"] == 0, counts
    print("tasks: test_task with --test-task only, arguments, spread over the "
          "loops, moved, cancelled, stopped, counted: PASS")


def check_plugin(binary, up, plugin):
    """askai loads the plugins that the configuration file lists.

    The test plugin (src/test_plugin.c) has five tools.  The check lists
    and calls test_echo, runs a call of test_state that fails as a client's
    job, grants test_echo to a role by name, reloads the configuration,
    calls it from a turn of an ask call, and then starts askai with files
    that askai must refuse.  Then it gives the plugin
    options, which test_config shows, and starts askai with options that
    askai must refuse.
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

    def address(name):
        """Returns the address of the dynamic symbol name.  The section
        header of type SHT_DYNSYM (11) gives the symbols, and its sh_link
        names the section of their names."""
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
        return address

    def offset(at):
        """Returns the file offset of the address at.  A PT_LOAD program
        header (1) maps the address to its file offset."""
        first, size, count = number(32, 8), number(54, 2), number(56, 2)
        for header in range(first, first + size * count, size):
            vaddr = number(header + 16, 8)
            if number(header, 4) == 1 and vaddr <= at < vaddr + number(header + 32, 8):
                return number(header + 8, 8) + at - vaddr
        raise AssertionError(f"the address {at:#x} is not in the file")

    def symbol(name):
        """Returns the file offset of the dynamic symbol name."""
        return offset(address(name))

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
            text = json.dumps(grouped({"url": up.url(), "man": True,
                                       "plugins": plugins} | options))
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

        # A role that names the plugin instead of its tools stops askai, and
        # the error lists the tools, the plugin's among them.
        write([plugin], roles={"r": {"tools": ["test"]}})
        run = subprocess.run([binary, "-l", "127.0.0.1:0", "-c", path],
                             stdin=subprocess.DEVNULL, capture_output=True, text=True,
                             timeout=5)
        assert run.returncode == 2 and 'role r: askai offers no tool "test"; the tools ' \
            'are ask_ai, man, test_echo, test_state, test_misuse, test_fetch, ' \
            'test_config\n' in run.stderr, run.stderr

        # Without roles every client may call the plugin's tool.  Its id
        # follows those of askai's own tools.  As askai starts, before its
        # endpoint, it logs the built-in tools, and the plugin with its path
        # and its tools.
        write([plugin])
        with Askai(binary, "-c", path) as ai:
            tools = ai.rpc("tools/list")[2]["result"]["tools"]
            assert [t["name"] for t in tools] == \
                ["ask_ai", "man", "test_echo", "test_state", "test_misuse",
                 "test_fetch", "test_config"], tools
            assert ai.listed("built-in tools for clients:") == [
                "ask_ai      on", "man         on", "git         off (--git PATH)",
                "notes       off (--notes DIR)"] + [
                f"{name:<10}  off (--client-jobs)"
                for name in ("background", "jobs", "wait", "timer")], ai.log
            assert ai.listed("plugin test:") == [
                f"file     {plugin}", "options  none", "tools    test_echo"] + [
                f"         {t['name']}" for t in tools[3:]], ai.log
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
            # A notification of the same client ends a plugin's call that
            # waits, at once.
            results = []
            thread = threading.Thread(target=lambda: results.append(ai.rpc(
                "tools/call", {"name": "test_state", "arguments": {"wait_ms": 3000}},
                rpc_id=7)))
            start = time.monotonic()
            thread.start()
            time.sleep(0.3)
            assert ai.rpc("notifications/cancelled", {"requestId": 7},
                          rpc_id=None)[0] == 202
            thread.join(5)
            assert results[0][0] == 202 and results[0][2] is None, results
            assert time.monotonic() - start < 1.5, "the call waited on"

        # With --client-jobs, background starts a call of a plugin's tool as
        # a job.  A call that fails with an error makes a failed job: a wait
        # and the list say so, and jobs answers with the error of a direct
        # call, in either era.
        with Askai(binary, "-c", path, "--client-jobs") as ai:
            args = {"wait_ms": 200, "fail": True}
            started = text_of(call(ai, "background", {"tool": "test_state",
                                                      "arguments": args})["result"])
            match = re.fullmatch(r"Started job ([0-9a-f]{32})\. Call wait to wait for "
                                 r"it, then jobs with the action result and this id\.",
                                 started)
            assert match, started
            jid = match.group(1)
            said = text_of(call(ai, "wait", {"seconds": 5, "jobs": [jid]})["result"])
            assert said == f"[askai: job {jid}, test_state, failed: call jobs with the " \
                f"action result and job {jid} for its result.]\n", said
            listed = text_of(call(ai, "jobs", {"action": "list"})["result"])
            assert re.fullmatch(f"job {jid}: test_state: failed [0-9] s ago\n", listed), \
                listed
            for rpc in (ai.rpc, ai.modern):
                direct = rpc("tools/call", {"name": "test_state", "arguments": args})[2]
                stored = rpc("tools/call", {"name": "jobs", "arguments": {
                    "action": "result", "job": jid}})[2]
                assert "error" in direct and stored["error"] == direct["error"], \
                    (stored, direct)

        # A role grants the tool by its name.  A reload reads the names
        # from the registry that askai started with.
        only = {"roles": {"echo": {"tools": ["test_echo"]}},
                "anonymous": {"loopback": "echo"}}
        write([plugin], **only)
        with Askai(binary, "-c", path) as ai:
            assert names(ai) == ["test_echo"]
            assert ai.listed("role echo:") == [
                "tools           test_echo", "metrics         no"], ai.log
            assert ai.listed("warning: no role lists these tools:") == [
                "ask_ai", "man", "test_state", "test_misuse", "test_fetch",
                "test_config"], ai.log
            assert text_of(call(ai, "test_echo", {"text": "a"})["result"]) == "a"
            assert call(ai, "man", {"name": "ls"})["error"]["message"] == \
                "Tool not available: man"
            write([plugin], roles={"both": {"tools": ["man", "test_echo"]}},
                  anonymous={"loopback": "both"})
            ai.log.clear()
            ai.proc.send_signal(signal.SIGHUP)
            ai.wait_log("configuration reloaded")
            # A reload logs the new policy.
            ai.wait_list("role both:", [
                "tools           man", "                test_echo", "metrics         no"])
            ai.wait_list("warning: no role lists these tools:", [
                "ask_ai", "test_state", "test_misuse", "test_fetch", "test_config"])
            assert not any("need a restart" in line for line in ai.log), ai.log
            assert names(ai) == ["man", "test_echo"]
            assert text_of(call(ai, "test_echo", {"text": "b"})["result"]) == "b"
            # A changed list of plugins takes a restart: the tool stays.
            write([], **only)
            ai.log.clear()
            ai.proc.send_signal(signal.SIGHUP)
            ai.wait_log(r"^askai: warning: other changed options need a restart$")
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
            assert names(ai) == ["ask_ai", "man", "test_echo", "test_state",
                                 "test_misuse", "test_fetch", "test_config"]
            # The kinds with TOOL_LENT can be lent, and those with
            # TOOL_DETACH can run in the background.
            assert ai.listed("policy: built in, without ASKAI_TOKEN")[3:] == [
                "upstream tools  man"] + [
                f"{'':14}  {name}" for name in ("test_echo", "test_state", "test_misuse",
                                                 "compact", "background", "jobs", "wait",
                                                 "timer")], ai.log
            count = len(up.requests)
            answer, trace = [c["text"] for c in ai.ask("Q")["content"]]
            first, second = up.requests[count:]
            assert [t["function"]["name"] for t in first["body"]["tools"]] == \
                ["man", "test_echo", "test_state", "test_misuse", "compact",
                 "background", "jobs", "wait", "timer"], first
            assert [m["content"] for m in second["body"]["messages"][2:]] == \
                ["The seconds argument must be a whole number from 1 to 3600.",
                 "x"], second
            assert answer == "A" and '- test_echo {"text":"x"}: 1 bytes\n' in trace, \
                trace
            assert total(ai.metrics(), "askai_turn_tool_calls_total",
                         tool="plugin") == 1
        up.reply = lambda request: (200, ANSWERS["openai"])

        # A role whose upstream may call test_echo alone, which has no
        # TOOL_DETACH, gets no background and no jobs, and a role without
        # the ask tool lends the upstream nothing, so no role lists man.
        write([plugin], roles={"e": {"tools": ["ask_ai"], "upstream": ["test_echo"]},
                               "s": {"tools": ["ask_ai"], "upstream": ["test_state"]},
                               "n": {"tools": ["test_misuse"], "upstream": ["man"]}},
              anonymous={"loopback": "e"}, **{"upstream-tools": True})
        with Askai(binary, "-c", path) as ai:
            assert ai.listed("role e:")[1:-1] == ["upstream tools  test_echo"] + [
                f"{'':14}  {name}" for name in ("compact", "wait", "timer")], ai.log
            assert ai.listed("role s:")[1:-1] == ["upstream tools  test_state"] + [
                f"{'':14}  {name}" for name in ("compact", "background", "jobs", "wait",
                                                 "timer")], ai.log
            assert ai.listed("role n:")[1:-1] == ["upstream tools  none"], ai.log
            assert ai.listed("warning: no role lists these tools:") == [
                "man", "test_fetch", "test_config"], ai.log

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
        # An element of "plugins" can be an object with the members path
        # and options.  configure() of the plugin checks the options, and
        # test_config shows the greeting that it stored.  A plugin without
        # options has the default greeting.  A reload changes no option.
        # definition() gets the configuration, so the description of
        # test_config in tools/list ends with the same greeting.
        def greeting(ai):
            tools = ai.rpc("tools/list")[2]["result"]["tools"]
            desc = next(t["description"] for t in tools if t["name"] == "test_config")
            text = text_of(call(ai, "test_config", {})["result"])
            word = text.removeprefix("greeting=")
            assert desc == f"Returns the greeting of the plugin's options ({word}).", \
                (desc, text)
            return text
        for plugins, expected in (([plugin], "greeting=hello"),
                                  ([{"path": plugin}], "greeting=hello"),
                                  ([{"path": plugin, "options": {}}], "greeting=hello"),
                                  ([{"path": plugin, "options": {"greeting": "hi"}}],
                                   "greeting=hi")):
            write(plugins)
            with Askai(binary, "-c", path) as ai:
                assert greeting(ai) == expected, (plugins, greeting(ai))
                given = "options" in plugins[0] if isinstance(plugins[0], dict) else False
                assert ai.listed("plugin test:")[:2] == [
                    f"file     {plugin}", f"options  {'given' if given else 'none'}"], ai.log
                if expected == "greeting=hi":
                    write([{"path": plugin, "options": {"greeting": "yo"}}])
                    ai.log.clear()
                    ai.proc.send_signal(signal.SIGHUP)
                    ai.wait_log(r"^askai: warning: other changed options need a restart$")
                    assert greeting(ai) == "greeting=hi"
        for plugins, message in (
                ([{"path": plugin, "options": 5}], f"askai: {path}: plugins: the "
                 "member options of a plugin is not an object"),
                ([{"path": plugin, "options": None}], f"askai: {path}: plugins: the "
                 "member options of a plugin is not an object"),
                ([{"path": plugin, "name": "x"}], f"askai: {path}: plugins: unknown "
                 "member of a plugin: name"),
                ([{"options": {}}], f"askai: {path}: plugins: a plugin has no member "
                 "path"),
                ([{"path": "test_plugin.so"}], f"askai: {path}: plugins: not an "
                 "absolute path: test_plugin.so"),
                ([{"path": plugin, "options": {"greeting": 5}}], f"askai: {plugin}: "
                 "options: greeting: not a string of at most 63 bytes"),
                ([{"path": plugin, "options": {"greeting": "x" * 64}}],
                 f"askai: {plugin}: options: greeting: not a string of at most 63 "
                 "bytes"),
                ([{"path": plugin, "options": {"colour": "red"}}], f"askai: {plugin}: "
                 "options: unknown option: colour")):
            fails(plugins, message)

        # A copy whose config_size is 0 has configure() without room for a
        # configuration.  In another copy, the dynamic relocation that sets
        # the pointer configure sets the member step of kind 7 instead,
        # which no tool has, and the pointer configure and config_size are
        # 0.  That plugin has no options.  The manifest's ABI value 0 is the
        # size of a pointer, its value 5 the size of struct tool_kind, whose
        # member step follows two pointers, its value 7 the size of struct
        # plugin, whose last member is config_size, and its value 16 the
        # offset of configure.
        size_at = symbol(b"askai_plugin") + word(8) - 8
        copy = bytearray(image)
        copy[size_at:size_at + 8] = bytes(8)
        with open(f"{d}/size.so", "wb") as f:
            f.write(copy)
        fails([f"{d}/size.so"], f"askai: {d}/size.so: its askai_plugin has "
              "configure() without a config_size of 1 to 4096")
        configure = address(b"askai_plugin") + word(17)
        spare = address(b"askai_plugin") + 7 * word(6) + 2 * word(1)
        first, size, count = number(40, 8), number(58, 2), number(60, 2)
        relocations = 0
        for header in range(first, first + size * count, size):
            # Only the sections of type SHT_RELA (4) have relocations with
            # addends.
            start = number(header + 24, 8)
            for rela in range(start, start + number(header + 32, 8), 24) \
                    if number(header + 4, 4) == 4 else ():
                if number(rela, 8) == configure:
                    copy[rela:rela + 8] = spare.to_bytes(8, sys.byteorder)
                    relocations += 1
        assert relocations == 1, relocations
        copy[offset(configure):offset(configure) + 8] = bytes(8)
        with open(f"{d}/bare.so", "wb") as f:
            f.write(copy)
        fails([{"path": f"{d}/bare.so", "options": {}}], f"askai: {d}/bare.so: "
              "options: the plugin has no options")
        write([f"{d}/bare.so"])
        with Askai(binary, "-c", path) as ai:
            assert greeting(ai) == "none"

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
          "askai refused; options given to the plugin, kept over a reload, and "
          "refused: PASS")


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
            text = json.dumps(grouped({"url": up.url(), "plugins": [plugin]} | options))
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


def check_misuse(binary, up, plugin):
    """askai goes on working when a plugin uses the call API in ways that
    src/tool.h and src/mcp.h do not allow, and nothing of one call reaches
    another.

    The kind of the tool test_misuse of the test plugin (src/test_plugin.c)
    has no step() and no drop().  Its case "ready" asks for a step, its case
    "own" sets the mcp_slot_own() pointer, and its case "after" ends the
    call and then calls each function of the call API that ends or changes
    a call.  The drop() of test_state tries to end its call.  A background
    job of test_state with "fail" ends with mcp_slot_fail() while the reply
    of its ask call is being sent.
    """
    plugin = os.path.realpath(plugin)
    auth = {"Authorization": "Bearer alice"}
    tools = ["ask_ai", "test_echo", "test_state", "test_misuse"]
    # The text and the error flag of each case's result.
    expected = {
        "ready": ("The tool's kind has no step(), so the call cannot go on.", True),
        "own": ("own", False),
        "after": ("first", False),
    }
    hold = up.reply
    with tempfile.TemporaryDirectory() as d:
        path = f"{os.path.realpath(d)}/askai.json"
        with open(path, "w") as f:
            json.dump(grouped({"url": up.url(), "plugins": [plugin],
                               "upstream-tools": True, "max-turns": 2,
                               "roles": {"all": {"tools": tools}},
                               "identities": {"alice": {
                                   "role": "all",
                                   "tokens": ["sha256:" + hashlib.sha256(b"alice").hexdigest()]}}}),
                      f)
        os.chmod(path, 0o600)
        with Askai(binary, "-c", path, loops=1) as ai:
            def call(name, arguments):
                status, _, body = ai.rpc("tools/call",
                                         {"name": name, "arguments": arguments},
                                         headers=auth)
                assert status == 200 and "result" in body, (status, body)
                return body["result"]

            def state():
                """The number of calls of alice's instance, and the text that
                it stored when the test_state call ended."""
                found = re.fullmatch(r"principal=\w+ instance=\S+ calls=(\d+) last=(.*)",
                                     text_of(call("test_state", {})), re.S)
                assert found
                return int(found[1]), found[2]

            for case, (text, failed) in expected.items():
                result = call("test_misuse", {"case": case})
                assert text_of(result) == text and \
                    bool(result.get("isError")) == failed, (case, result)
                # The next call on the loop gets only its own output.
                assert text_of(call("test_echo", {"text": "x"})) == "x", case
            # Each exported function refuses the invalid inputs that the case
            # "invalid" passes it.
            found = re.fullmatch(r"refused (\d+) of (\d+)",
                                 text_of(call("test_misuse", {"case": "invalid"})))
            assert found and found[1] == found[2] and int(found[2]) >= 39, found
            assert text_of(call("test_echo", {"text": "x"})) == "x"
            # A handle of the loop's table gives a call slot only if it was
            # made for one.  The call's own request also has handles there.
            assert text_of(call("test_misuse", {"case": "handles"})) == \
                "foreign 0 own 1"
            # After the end of the call, each function that would end or
            # change the call, and mcp_slot_fetched(), refused with EINVAL.
            assert state()[1] == "refused 10 of 10"

            # The same three cases in a turn of an ask call.
            up.reply = lambda request: (200, turn_stream(
                [(case, "test_misuse", json.dumps({"case": case})) for case in expected]
                if len(request["body"]["messages"]) == 1 else (), text="A"))
            count = len(up.requests)
            result = call("ask_ai", {"prompt": "Q"})
            up.reply = hold
            # The result has the answer and then the trace of the turns.
            assert result["content"][0]["text"] == "A" and \
                not result.get("isError"), result
            told = [m["content"] for m in up.requests[count:][1]["body"]["messages"][2:]]
            assert len(told) == 3 and all(
                said.startswith(text) for said, (text, _) in zip(told, expected.values())), told

            # The client of a call that waits closes its connection, so askai
            # drops the call.  The drop() of test_state tries to end the
            # call, and askai ignores that.
            calls = state()[0]
            with socket.create_connection(("127.0.0.1", ai.port), 10) as sock:
                sock.sendall(rpc_request(
                    1, {"name": "test_state", "arguments": {"wait_ms": 60000}},
                    "tools/call", headers=auth))
                # Each call of state() adds 1 to the number of calls, and the
                # call that waits adds 1 when it starts.  So it has started
                # when the number grows by 2 between two calls of state().
                end = time.monotonic() + 5
                while (now := state()[0]) != calls + 2:
                    assert now == calls + 1 and time.monotonic() < end, (calls, now)
                    calls = now
            wait_for(lambda: state()[1] == "dropped")
            assert text_of(call("test_echo", {"text": "y"})) == "y"

            # A background job of a turn fails with mcp_slot_fail() while
            # the reply of its ask call is being sent.  The reply is larger
            # than the buffers of the connection, and the client reads only
            # the head of the reply before the job fails, so askai is still
            # sending the reply then.  The job's failure must not change
            # the reply.
            big = "B" * (6 << 20)
            job = {"tool": "test_state", "arguments": {"wait_ms": 2000, "fail": True}}
            up.reply = lambda request: (200, turn_stream(
                [("j", "background", json.dumps(job))]
                if len(request["body"]["messages"]) == 1 else (), text=big))
            count = len(up.requests)
            with socket.socket() as sock:
                sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
                sock.settimeout(30)
                sock.connect(("127.0.0.1", ai.port))
                sock.sendall(rpc_request(
                    1, {"name": "ask_ai", "arguments": {"prompt": "Q"}},
                    "tools/call", headers=auth))
                # The job starts before the second turn's upstream request.
                wait_for(lambda: len(up.requests) == count + 2)
                fails_at = time.monotonic() + 2.0
                replies = Replies(sock)
                while b"\r\n\r\n" not in replies.buf:
                    assert replies.fill(len(replies.buf) + 1)
                assert time.monotonic() < fails_at, "the reply began after the job failed"
                time.sleep(fails_at + 0.5 - time.monotonic())
                status, body = replies.next()
            up.reply = hold
            assert status == 200 and not body["result"].get("isError") and \
                body["result"]["content"][0]["text"] == big, (status, str(body)[:300])
            assert text_of(call("test_echo", {"text": "z"})) == "z"
    print("misuse: askai ignores or refuses a step without step(), an own word "
          "without drop(), the call API after the end and from drop(), and a "
          "job that fails while its ask call's reply is sent, and the next "
          "calls work: PASS")


def check_web(binary, up, plugin):
    """A call of a plugin's tool sends HTTP requests through askai, which can
    store the responses for equal requests (src/web.h).

    The tool test_fetch of the test plugin (src/test_plugin.c) sends the
    request that its arguments give with mcp_slot_fetch(), and writes what
    mcp_slot_fetched() stores.  A local server answers the requests.  It
    counts them by path and query, and it records the headers and the body
    of the last request of each.  The path selects the response: /slow
    waits 0.5 s, /sleep waits 3 s, /hang answers only after 5 s and notes
    when the client closes the connection, /fail is a 500 error with
    Retry-After, /big has 1000 bytes, /echo echoes the body, /gzip has 100000
    bytes, compressed with gzip if the request accepts that, and any other
    path is a short text.
    """
    plugin = os.path.realpath(plugin)
    seen = {}
    last = {}
    closed = []
    lock = threading.Lock()

    class Site(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *args):
            pass

        def answer(self, status, body, ctype="text/plain; charset=utf-8",
                   headers=()):
            self.send_response(status)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            for name, value in headers:
                self.send_header(name, value)
            self.end_headers()
            self.wfile.write(body)

        def hang(self):
            """Waits up to 5 s for the client to close the connection."""
            end = time.monotonic() + 5
            while time.monotonic() < end:
                ready, _, _ = select.select([self.connection], [], [], 0.05)
                if ready and not self.connection.recv(1, socket.MSG_PEEK):
                    with lock:
                        closed.append(time.monotonic())
                    self.close_connection = True
                    return
            self.answer(200, b"late")

        def serve(self, body):
            with lock:
                seen[self.path] = seen.get(self.path, 0) + 1
                last[self.path] = (self.headers, body)
            name = self.path.split("?")[0]
            if name == "/hang":
                self.hang()
                return
            time.sleep({"/slow": 0.5, "/sleep": 3}.get(name, 0))
            if name == "/fail":
                self.answer(500, b"no", headers=[("Retry-After", "7")])
            elif name == "/big":
                self.answer(200, b"x" * 1000)
            elif name == "/gzip" and "gzip" in self.headers.get("Accept-Encoding", ""):
                self.answer(200, gzip.compress(b"x" * 100000),
                            headers=[("Content-Encoding", "gzip")])
            elif name == "/gzip":
                self.answer(200, b"x" * 100000)
            elif name == "/echo":
                self.answer(200, body, self.headers.get("Content-Type", ""))
            else:
                self.answer(200, b"hello")

        def do_GET(self):
            self.serve(b"")

        def do_POST(self):
            self.serve(self.rfile.read(int(self.headers.get("Content-Length", 0))))

    def count(path):
        with lock:
            return seen.get(path, 0)

    server = QuietServer(("127.0.0.1", 0), Site)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    base = f"http://127.0.0.1:{server.server_address[1]}"

    def call(ai, rpc_id=1, **arguments):
        status, _, body = ai.rpc("tools/call", {"name": "test_fetch",
                                                "arguments": arguments}, rpc_id=rpc_id)
        assert status == 200 and "result" in body, (status, body)
        return body["result"]

    def fetch(ai, **arguments):
        """The fields of the response that test_fetch writes, and its body."""
        result = call(ai, **arguments)
        assert not result.get("isError"), (arguments, result)
        head, _, text = text_of(result).partition("\n\n")
        fields = dict((line + " ").split(" ", 1) for line in head.split("\n"))
        fields = {k: v[:-1] for k, v in fields.items()}
        return fields, text

    def relieve(ai, pressure):
        """Sets the memory pressure of the test aid to pressure until a relief
        is over, and then to 0."""
        trims = total(ai.metrics(), "askai_memory_trims_total")
        set_pressure(level, pressure)
        wait_for(lambda: total(ai.metrics(), "askai_memory_trims_total") > trims)
        set_pressure(level, 0)
        wait_for(lambda: ai.metrics()["askai_memory_pressure_level"] == 0 or
                 not memory_calm())

    level = tempfile.NamedTemporaryFile("w", prefix="askai-pressure-")
    set_pressure(level, 0)
    try:
        with tempfile.TemporaryDirectory() as d:
            path = f"{os.path.realpath(d)}/askai.json"
            with open(path, "w") as f:
                json.dump(grouped({"url": up.url(), "plugins": [plugin]}), f)
            os.chmod(path, 0o600)
            with Askai(binary, "-c", path, "--verbose", "--test-pressure",
                       level.name) as ai:
                # A GET has the Accept of the call and the User-Agent of
                # askai.
                fields, text = fetch(ai, url=f"{base}/text?get", accept="text/plain")
                assert fields == {"status": "200", "type": "text/plain; charset=utf-8",
                                  "age": "0", "retry": "0", "e": "0", "error": ""} and \
                    text == "hello", (fields, text)
                headers, _ = last["/text?get"]
                assert headers["User-Agent"] == "askai (plugin test)" and \
                    headers["Accept"] == "text/plain", dict(headers)
                ai.wait_log(r"^askai: web request GET http://127\.0\.0\.1:\d+/text\?get "
                            r"answered with status 200 after \d+ ms \(5 bytes\)$")

                # A POST sends the body as JSON.
                fields, text = fetch(ai, url=f"{base}/echo?post", body='{"a":1}')
                headers, sent = last["/echo?post"]
                assert fields["status"] == "200" and fields["type"] == \
                    "application/json" and text == '{"a":1}', (fields, text)
                assert headers["Content-Type"] == "application/json" and \
                    sent == b'{"a":1}', (dict(headers), sent)

                # With keep, an equal request gets the stored response.  A
                # request with another Accept, max or body is sent.
                for _ in range(2):
                    fields, text = fetch(ai, url=f"{base}/text?keep", keep=60)
                    assert text == "hello" and int(fields["age"]) >= 0, fields
                assert count("/text?keep") == 1
                for n, other in enumerate(({"accept": "text/html"}, {"max": 100},
                                           {"body": "{}"}), 2):
                    fetch(ai, url=f"{base}/text?keep", keep=60, **other)
                    assert count("/text?keep") == n, (other, count("/text?keep"))

                # If two equal requests are under way at once, one reaches
                # the server, and both calls get the response.
                out = []
                threads = [threading.Thread(target=lambda: out.append(fetch(
                    ai, url=f"{base}/slow?both", keep=60))) for _ in range(2)]
                for thread in threads:
                    thread.start()
                    time.sleep(0.1)
                for thread in threads:
                    thread.join(5)
                assert [text for _, text in out] == ["hello", "hello"], out
                assert count("/slow?both") == 1

                # An equal request with another timeout does not wait for the
                # request under way.  It is sent.
                out = []
                threads = [threading.Thread(target=lambda t=t: out.append(fetch(
                    ai, url=f"{base}/slow?timeouts", keep=60, timeout=t)))
                    for t in (5, 6)]
                for thread in threads:
                    thread.start()
                    time.sleep(0.1)
                for thread in threads:
                    thread.join(5)
                assert [text for _, text in out] == ["hello", "hello"], out
                assert count("/slow?timeouts") == 2

                # askai asks for a compressed body and decodes it.  max is the
                # length of the decoded body.
                fields, text = fetch(ai, url=f"{base}/gzip?all", max=100000)
                headers, _ = last["/gzip?all"]
                assert fields["e"] == "0" and text == "x" * 100000 and \
                    "gzip" in headers["Accept-Encoding"], (fields, dict(headers))
                fields, text = fetch(ai, url=f"{base}/gzip?less", max=99999)
                assert fields["e"] == str(errno.EFBIG) and text == "", fields

                # A stored response is used only while it is younger than
                # keep.
                fetch(ai, url=f"{base}/text?short", keep=1)
                fetch(ai, url=f"{base}/text?short", keep=1)
                assert count("/text?short") == 1
                time.sleep(1.1)
                fetch(ai, url=f"{base}/text?short", keep=1)
                assert count("/text?short") == 2

                # Under high memory pressure, askai frees the stored responses
                # whose keep has passed, and under critical pressure all of
                # them.  A request with a longer keep could use them before.
                fetch(ai, url=f"{base}/text?old", keep=1)
                fetch(ai, url=f"{base}/text?new", keep=60)
                time.sleep(1.1)
                relieve(ai, 1)
                fetch(ai, url=f"{base}/text?old", keep=60)
                fetch(ai, url=f"{base}/text?new", keep=60)
                assert count("/text?old") == 2 and \
                    (count("/text?new") == 1 or not memory_calm()), seen
                relieve(ai, 2)
                fetch(ai, url=f"{base}/text?new", keep=60)
                assert count("/text?new") == 2, seen

                # A response with an error status is not stored.
                for _ in range(2):
                    fields, text = fetch(ai, url=f"{base}/fail?keep", keep=60)
                    assert fields["status"] == "500" and fields["retry"] == "7" and \
                        fields["e"] == "0" and text == "no", (fields, text)
                assert count("/fail?keep") == 2

                # A body longer than max fails with EFBIG.  A body of max
                # bytes comes whole.
                fields, text = fetch(ai, url=f"{base}/big?999", max=999)
                assert fields["e"] == str(errno.EFBIG) and text == "" and \
                    fields["error"] == "The response body is longer than 999 bytes.", \
                    fields
                fields, text = fetch(ai, url=f"{base}/big?1000", max=1000)
                assert fields["e"] == "0" and text == "x" * 1000, fields

                # A request that takes longer than its timeout fails with
                # ETIMEDOUT at its deadline.
                start = time.monotonic()
                fields, text = fetch(ai, url=f"{base}/sleep?late", timeout=1)
                took = time.monotonic() - start
                assert fields["e"] == str(errno.ETIMEDOUT) and fields["status"] == "0", \
                    fields
                assert took < 1.8, f"the call took {took:.2f} s"

                # mcp_slot_fetch() refuses what web_request does not allow.
                for arguments in ({"url": "ftp://127.0.0.1/"},
                                  {"url": "file:///etc/passwd"},
                                  {"url": "http://user:pw@127.0.0.1/"},
                                  {"url": "http://127.0.0.1/a b"},
                                  {"url": "http:///nohost"},
                                  {"url": "http://127.0.0.1/" + "a" * 4096},
                                  {"url": base, "accept": "a\nb"},
                                  {"url": base, "accept": ""},
                                  {"url": base, "max": 0},
                                  {"url": base, "max": (16 << 20) + 1},
                                  {"url": base, "timeout": 0},
                                  {"url": base, "timeout": 301},
                                  {"url": base, "keep": 7 * 86400 + 1}):
                    result = call(ai, **arguments)
                    assert result.get("isError") and \
                        text_of(result) == f"fetch: {errno.EINVAL}", (arguments, result)

                # A second request while the first is under way is refused.
                result = call(ai, url=f"{base}/slow?twice", twice=True)
                assert not result.get("isError") and \
                    text_of(result) == f"twice: {errno.EBUSY}", result

                # A notification ends a call that waits for its response,
                # and home closes the connection of the request.
                results = []
                thread = threading.Thread(target=lambda: results.append(ai.rpc(
                    "tools/call", {"name": "test_fetch",
                                   "arguments": {"url": f"{base}/hang?cancel"}},
                    rpc_id=7)))
                thread.start()
                wait_for(lambda: count("/hang?cancel") == 1)
                cancelled = time.monotonic()
                assert ai.rpc("notifications/cancelled", {"requestId": 7},
                              rpc_id=None)[0] == 202
                thread.join(5)
                assert results[0][0] == 202 and results[0][2] is None, results
                wait_for(lambda: closed, 1.5)
                assert closed[0] - cancelled < 1.5, closed[0] - cancelled
                assert fetch(ai, url=f"{base}/text?after")[1] == "hello"

                # A request whose keep is not 0 goes on without its call, and
                # askai stops at once with it under way.
                thread = threading.Thread(target=lambda: results.append(ai.rpc(
                    "tools/call", {"name": "test_fetch",
                                   "arguments": {"url": f"{base}/hang?stop",
                                                 "keep": 60}})), daemon=True)
                thread.start()
                wait_for(lambda: count("/hang?stop") == 1)
                stopping = time.monotonic()
            assert time.monotonic() - stopping < 3, "askai took long to stop"
            wait_for(lambda: len(closed) == 2, 2)
    finally:
        level.close()
        server.shutdown()
        server.server_close()
    print("web: GET and POST through askai, stored responses for equal requests, "
          "one request for two equal calls, keep, memory pressure, errors not "
          "stored, EFBIG, ETIMEDOUT, refused requests, EBUSY, cancellation and a "
          "stop with a request under way: PASS")


def ce_compiler(cid, name, kind, families, version, isa, track="stable"):
    """An element of a list of compilers of the mock Compiler Explorer."""
    return {"id": cid, "name": name, "compilerType": kind,
            "compilerCategories": families, "semver": version,
            "instructionSet": isa, "releaseTrack": track}


# The ids of compilers of the mock Compiler Explorer that answer in a way of
# their own (MockCE.compile).  Each of them has the compilerType of GCC.
CE_SPECIAL = ("gone", "boom", "busy", "later", "slow", "trunc", "big")

CE_LANGUAGES = [{"id": "c", "name": "C", "extensions": [".c"], "monaco": "nc"},
                {"id": "c++", "name": "C++", "extensions": [".cpp"],
                 "monaco": "cppp"}]

CE_COMPILERS = {
    "c": [ce_compiler("cg142", "x86-64 gcc 14.2", "", ["gcc"], "14.2", "amd64"),
          ce_compiler("cclang19", "x86-64 clang 19.1", "clang", ["clang", "llvm"],
                      "19.1", "amd64"),
          ce_compiler("cvc19", "x64 msvc 19.40", "win32-vc", ["msvc"], "19.40",
                      "amd64"),
          ce_compiler("cclangcl19", "x64 clang-cl 19.1", "clang-cl", [], "19.1",
                      "amd64"),
          ce_compiler("cti", "TI CL430 21.6", "ticlang", [], "21.6", "msp430"),
          ce_compiler("cgtrunk", "x86-64 gcc (trunk)", "", ["gcc"], "(trunk)",
                      "amd64", "nightly"),
          {"name": "a row without an id"}] +
         [ce_compiler(cid, "special " + cid, "", ["gcc"], "1.0", "amd64")
          for cid in CE_SPECIAL],
    "c++": [ce_compiler("cppg142", "x86-64 g++ 14.2", "", ["gcc"], "14.2", "amd64"),
            ce_compiler("cvc19", "x64 msvc 19.40", "win32-vc", ["msvc"], "19.40",
                        "amd64")],
    # A compiler with more family tags than the plugin takes, one with a kind
    # longer than it takes, and one whose tags repeat.
    "tags": [ce_compiler("many", "many tags", "", [f"t{i}" for i in range(65)], "1.0",
                         "amd64"),
             ce_compiler("longkind", "long kind", "k" * 129, [], "1.0", "amd64"),
             ce_compiler("few", "few tags", "", ["a", "b", "a"], "1.0", "amd64")],
    # An element larger than the plugin takes.
    "huge": [ce_compiler("long", "x" * (4 << 10), "", [], "1.0", "amd64")],
    # More different kinds than the counts take.
    "keys": [ce_compiler(f"k{i}", "kind", f"kind{i}", [], "1.0", "amd64")
             for i in range(257)],
    # A compiler whose probe has a macro value of more than 256 bytes.
    "probe": [ce_compiler("longvalue", "long value", "", [], "1.0", "amd64")],
}

# The macros in the output of the probe of godbolt_targets: those of an
# x86_64 Linux GCC with glibc, and those of an x64 MSVC.
CE_PROBE_GNU = {"__x86_64__": "1", "__linux__": "1", "__SIZEOF_INT__": "4",
                "__SIZEOF_LONG__": "8", "__SIZEOF_POINTER__": "8",
                "__CHAR_BIT__": "8", "__LP64__": "1", "__BYTE_ORDER__": "1234",
                "__ORDER_LITTLE_ENDIAN__": "1234", "__GLIBC__": "2",
                "__GNUC__": "14", "__GNUC_MINOR__": "2",
                "__STDC_VERSION__": "201710L"}
CE_PROBE_MSVC = {"_M_X64": "100", "_WIN32": "1", "_WIN64": "1", "_MSC_VER": "1940"}
CE_PROBE_HEADERS = ("limits.h", "features.h", "TargetConditionals.h", "_mingw.h")


class MockCE:
    """A local Compiler Explorer for check_godbolt, on an ephemeral port.

    It records each request as (method, path, query, headers, body), with
    the path percent-decoded.  GET /api/languages and GET /api/compilers/LANG
    answer with CE_LANGUAGES and CE_COMPILERS.  POST /api/compiler/ID/compile
    answers by the id: gone is 404, boom is 500 with a JSON message, busy is
    429 with Retry-After 1 for its first request, later is 429 with
    Retry-After 120, slow waits 3 s, trunc has cut assembly, big has more
    than 4 MiB of assembly, and euro has 3.9 MB of assembly in one string of
    the 3-byte character U+20AC.  longline has a message line of 3 MB,
    labels has 200,000 label definitions, and array answers a JSON array.
    Any other id compiles: each line of the source becomes a line of
    assembly, -Wall adds a warning, and -DFAIL adds an error and the exit
    code 1.  With producePp, the text of the preprocessor is the source
    after a line marker, or a macro with -dM.  With /EP or /clang:-dM,
    stdout has the source or a macro.  The probe of godbolt_targets gets
    the block of CE_PROBE_GNU, of CE_PROBE_MSVC for cvc19, of CE_PROBE_GNU
    with a value of 257 bytes for longvalue, or a block without its end for
    cti.  POST /api/shortener answers a JSON object with sessions with a
    link whose id is a hash of the body, and answers 500 if the body is not
    such an object or has the word NOSHORT.  Any other request gets 404.
    """

    def __init__(self):
        self.lock = threading.Lock()
        self.requests = []
        self.busy = 0
        ce = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *args):
                pass

            def do_GET(self):
                ce.serve(self, b"")

            def do_POST(self):
                ce.serve(self, self.rfile.read(int(self.headers.get("Content-Length", 0))))

        self.server = QuietServer(("127.0.0.1", 0), Handler)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def url(self):
        return f"http://127.0.0.1:{self.server.server_address[1]}"

    def close(self):
        self.server.shutdown()
        self.server.server_close()

    def seen(self, method=None, path=None):
        """The requests so far, of @p method and @p path if they are given."""
        with self.lock:
            return [r for r in self.requests
                    if method in (None, r[0]) and path in (None, r[1])]

    def bodies(self, cid=None):
        """The bodies of the compilations so far, of the compiler @p cid if it
        is given, as JSON."""
        return [json.loads(r[4]) for r in self.seen("POST")
                if r[1].startswith("/api/compiler/") and
                cid in (None, r[1].split("/")[3])]

    def serve(self, handler, body):
        raw, _, query = handler.path.partition("?")
        path = urllib.parse.unquote(raw)
        with self.lock:
            self.requests.append((handler.command, path, query, handler.headers, body))
        status, data, headers = self.answer(handler.command, path, handler.headers, body)
        out = data if isinstance(data, bytes) else json.dumps(data).encode()
        handler.send_response(status)
        handler.send_header("Content-Type", "text/plain; charset=utf-8"
                            if isinstance(data, bytes) else "application/json")
        handler.send_header("Content-Length", str(len(out)))
        for name, value in headers:
            handler.send_header(name, value)
        handler.end_headers()
        try:
            handler.wfile.write(out)
        except OSError:
            pass

    def answer(self, method, path, headers, body):
        if method == "POST" and path == "/api/shortener":
            try:
                state = json.loads(body)
            except ValueError:
                state = None
            if headers.get("Content-Type") != "application/json" or \
                    not isinstance(state, dict) or "sessions" not in state:
                return 500, b"Missing config parameter", ()
            if b"NOSHORT" in body:
                return 500, b"Storage is broken", ()
            return 200, {"url": f"{self.url()}/z/{hashlib.sha256(body).hexdigest()[:10]}"}, ()
        if method == "GET" and path == "/api/languages":
            return 200, CE_LANGUAGES, ()
        if method == "GET" and path == "/api/compilers/broken":
            return 500, {"error": True, "message": "No list today"}, ()
        if method == "GET" and path == "/api/compilers/odd":
            return 200, {"a": 1}, ()
        if method == "GET" and path.startswith("/api/compilers/"):
            return 200, CE_COMPILERS.get(path.removeprefix("/api/compilers/"), []), ()
        match = re.fullmatch(r"/api/compiler/([^/]+)/compile", path)
        if method == "POST" and match:
            return self.compile(match[1], json.loads(body))
        return 404, b"Not Found", ()

    def compile(self, cid, body):
        args = body["options"]["userArguments"]
        with self.lock:
            self.busy += cid == "busy"
            first = self.busy == 1
        if cid == "gone":
            return 404, b"Not Found", ()
        if cid == "boom":
            return 500, {"error": True, "message": "Something broke"}, ()
        if cid == "later" or (cid == "busy" and first):
            return 429, b"Too Many Requests", [("Retry-After",
                                                 "120" if cid == "later" else "1")]
        if cid == "slow":
            time.sleep(3)
        if cid == "trunc":
            return 200, {"code": 0, "stdout": [], "stderr": [], "truncated": True,
                         "asm": [{"text": "mov eax, 1"},
                                 {"text": "[truncated; too many lines]"}]}, ()
        if cid == "big":
            return 200, {"code": 0, "stdout": [], "stderr": [],
                         "asm": [{"text": "x" * 1023}] * 4200}, ()
        if cid == "euro":
            return 200, json.dumps({"code": 0, "stdout": [], "stderr": [],
                                    "asm": "\u20ac" * 1_300_000},
                                   ensure_ascii=False).encode(), ()
        if cid == "longline":
            return 200, {"code": 0, "stdout": [], "asm": [],
                         "stderr": [{"text": "y" * 3_000_000}]}, ()
        if cid == "labels":
            return 200, {"code": 0, "stdout": [], "stderr": [], "asm": [],
                         "labelDefinitions": {f"L{i}": i for i in range(200_000)}}, ()
        if cid == "array":
            return 200, [0] * 300_000, ()
        pp = "producePp" in body["options"]["compilerOptions"]
        lines = body["source"].splitlines()
        if "GBPROBE_BEGIN_1" in body["source"]:
            lines = self.probe(cid, body["source"])
        elif pp or "/EP" in args or "/clang:-dM" in args:
            lines = ["#define __GNUC__ 14" if pp else "#define _MSC_VER 1940"] \
                if "-dM" in args else ['# 1 "<source>"'] * pp + lines
        if pp:
            return 200, {"code": 0, "stdout": [], "stderr": [], "asm": [], "ppOutput": {
                "output": "".join(line + "\n" for line in lines),
                "numberOfLinesFiltered": 0}}, ()
        if "/EP" in args or "/clang:-dM" in args:
            return 200, {"code": 0, "stdout": [{"text": line} for line in lines],
                         "stderr": [], "asm": []}, ()
        stderr = []
        if "-Wall" in args:
            stderr.append({"text": "<source>:1:1: warning: a warning [-Wall]"})
        if "-DFAIL" in args:
            stderr.append({"text": "<source>:1:1: error: FAIL is defined"})
        return 200, {"code": 1 if "-DFAIL" in args else 0, "stdout": [],
                     "stderr": stderr,
                     "asm": [{"text": "asm: " + line, "source": None}
                             for line in lines]}, ()

    @staticmethod
    def probe(cid, source):
        """The lines of the output of the probe for the compiler @p cid."""
        if cid == "cti":
            return ["GBPROBE_BEGIN_1"]
        status = "not-requested"
        if source.startswith("#define GODBOLT_PROBE_HEADERS 1\n"):
            status = "included"
        macros = CE_PROBE_MSVC if cid == "cvc19" else CE_PROBE_GNU
        if cid == "longvalue":
            macros = CE_PROBE_GNU | {"__GNUC__": "1" * 257}
        return ["# 1 \"<source>\"", "GBPROBE_BEGIN_1"] + \
            [f"GBPROBE_HEADER({json.dumps(name)}, {json.dumps(status)})"
             for name in CE_PROBE_HEADERS] + \
            [f"GBPROBE_MACRO({json.dumps(name)}, {json.dumps(value)})"
             for name, value in macros.items()] + ["GBPROBE_END_1"]


def ce_records(text):
    """The records of the output of a compilation, as (id, args, status,
    body), and the last line."""
    head, _, last = text.removesuffix("\n").rpartition("\n")
    parts = re.split(r"^=== (\S+) (\"(?:[^\"\\]|\\.)*\"): (.*) ===\n", head + "\n",
                     flags=re.M)
    assert parts[0] == "", text
    return [(parts[i], json.loads(parts[i + 1]), parts[i + 2], parts[i + 3])
            for i in range(1, len(parts), 4)], last + "\n"


def check_godbolt(binary, up, godbolt):
    """The godbolt plugin (src/godbolt.c) against a local mock of Compiler
    Explorer (MockCE).

    The check lists the plugin's tools, and calls godbolt_catalog with each
    action and its filters.  It reads the output of a call from an offset,
    checks that askai sends each list once, and checks lists with malformed
    and oversized elements.  Then it compiles with
    several compilers and argument strings, and it checks the bodies of the
    requests, the records of failed requests, a retry after 429, cut and
    oversized responses, the end of the output at 14 MiB, a long line,
    large and odd responses, the stored responses of a call with offset,
    refused arguments and a cancelled call.  It preprocesses, outputs the
    predefined macros and probes the targets with compilers of each kind of
    driver.  It makes links with each value of the option share.  Last, it
    starts askai with options that the plugin refuses.
    """
    godbolt = os.path.realpath(godbolt)
    ce = MockCE()

    def call(ai, name, rpc_id=1, **arguments):
        status, _, body = ai.rpc("tools/call", {"name": name, "arguments": arguments},
                                 rpc_id=rpc_id)
        assert status == 200 and "result" in body, (status, body)
        return body["result"]

    def ok(ai, name, **arguments):
        result = call(ai, name, **arguments)
        assert not result.get("isError"), (arguments, result)
        return text_of(result)

    def refused(ai, name, **arguments):
        result = call(ai, name, **arguments)
        assert result.get("isError"), (arguments, result)
        return text_of(result)

    def gets(path):
        return len(ce.seen("GET", path))

    filters = {"binary": False, "binaryObject": False, "commentOnly": True,
               "debugCalls": False, "demangle": True, "directives": True,
               "execute": False, "intel": True, "labels": True,
               "libraryCode": False, "trim": False}
    source = "int square(int x)\n{\n\treturn x * x;\n}\n"
    asm = "".join(f"asm: {line}\n" for line in source.splitlines())
    warning = "<source>:1:1: warning: a warning [-Wall]\n"
    def summary(n, zero, other, failed, unsent):
        return f"Of {n} compilation{'s' * (n != 1)}, {zero} ended with exit code 0, " \
            f"{other} ended with another exit code or a timeout, {failed} ended with a " \
            f"failed request, and {unsent} ended without a request.\n"

    def left(n):
        return f"The plugin left out {n} element{'s' * (n != 1)} of the list. An " \
            "element must have a string member id, at most 64 family tags, and at most " \
            "128 bytes in its kind, its architecture and each family tag."

    try:
        with tempfile.TemporaryDirectory() as d:
            path = f"{os.path.realpath(d)}/askai.json"

            def write(options):
                with open(path, "w") as f:
                    json.dump(grouped({"url": up.url(),
                                       "plugins": [{"path": godbolt, "options": options}]}), f)
                os.chmod(path, 0o600)

            # A trailing '/' of the option url is left out.
            write({"url": ce.url() + "/"})
            with Askai(binary, "-c", path) as ai:
                tools = ai.rpc("tools/list")[2]["result"]["tools"]
                assert [t["name"] for t in tools] == \
                    ["ask_ai", "godbolt_catalog", "godbolt_compile", "godbolt_preprocess",
                     "godbolt_macros", "godbolt_targets"], tools
                assert [t["inputSchema"]["required"] for t in tools[1:]] == \
                    [["action"], ["compilers", "source"], ["compilers", "source"],
                     ["compilers"], ["compilers"]], tools
                for tool in tools[1:]:
                    assert f"Compiler Explorer at {ce.url()}" in tool["description"], tool
                # With short links, the tools that take share store data on
                # Compiler Explorer.
                for tool in tools[1:]:
                    shares = tool["name"] in ("godbolt_compile", "godbolt_preprocess",
                                              "godbolt_macros")
                    assert ("share" in tool["inputSchema"]["properties"]) == shares and \
                        tool["annotations"]["readOnlyHint"] != shares and \
                        ("Compiler Explorer stores the source permanently" in
                         tool["description"]) == shares, tool

                # The languages, with the User-Agent of askai and the Accept
                # that Compiler Explorer needs for JSON.
                assert ok(ai, "godbolt_catalog", action="languages") == \
                    "id\tname\nc\tC\nc++\tC++\nListed 2 of 2 matching languages. " \
                    "Compiler Explorer has 2 languages.\n"
                (_, _, _, headers, _), = ce.seen("GET", "/api/languages")
                assert headers["User-Agent"] == "askai (plugin godbolt)" and \
                    headers["Accept"] == "application/json", dict(headers)

                # The compilers of a language, with filters.  The list has
                # one element without an id, which the footer counts.
                fields = "id,name,compilerType,compilerCategories,semver," \
                    "instructionSet,releaseTrack"
                text = ok(ai, "godbolt_catalog", action="compilers")
                (_, _, query, _, _), = ce.seen("GET", "/api/compilers/c")
                assert query == "fields=" + fields, query
                lines = text.split("\n")
                assert lines[0] == "id\tname\tkind\tversion\tarchitecture\ttrack\tfamilies" \
                    and lines[1:3] == [
                        "cg142\tx86-64 gcc 14.2\tdefault\t14.2\tamd64\tstable\tgcc",
                        "cclang19\tx86-64 clang 19.1\tclang\t19.1\tamd64\tstable\tclang,llvm"] \
                    and lines[4] == "cclangcl19\tx64 clang-cl 19.1\tclang-cl\t19.1\tamd64\t" \
                    "stable\tunknown" and lines[-3:] == [left(1), "Listed 13 of 13 matching "
                                                          "compilers. The list of language c "
                                                          "has 13 compilers.", ""], text

                def ids(**arguments):
                    text = ok(ai, "godbolt_catalog", action="compilers", **arguments)
                    return [line.split("\t")[0] for line in text.split("\n")[1:]
                            if "\t" in line]

                assert ids(kind="default") == ["cg142", "cgtrunk", *CE_SPECIAL]
                assert ids(kind="clang") == ["cclang19"]
                assert ids(family="llvm") == ["cclang19"]
                assert ids(family="unknown") == ["cclangcl19", "cti"]
                assert ids(architecture="msp430") == ["cti"]
                assert ids(match="GCC trunk") == ["cgtrunk"]
                assert ids(ids=["cti", "cg142"]) == ["cg142", "cti"]
                assert refused(ai, "godbolt_catalog", action="compilers",
                               ids=["cvc19", "nope", "cti"], kind="default") == \
                    "These ids are not in the list of language c, or the filters " \
                    "exclude them: cvc19, nope, cti."
                assert refused(ai, "godbolt_catalog", action="languages", ids=["c", "d"]) \
                    == "These ids are not in the list of languages, or the filters " \
                    "exclude them: d."
                assert refused(ai, "godbolt_catalog", action="languages", kind="x") == \
                    "kind, family and architecture are not valid with the action languages."
                assert ok(ai, "godbolt_catalog", action="kinds") == \
                    "kind\tcompilers\nclang\t1\nclang-cl\t1\ndefault\t9\nticlang\t1\n" \
                    "win32-vc\t1\n" + left(1) + "\nListed 5 of the 5 kinds of the 13 " \
                    "matching compilers. The list of language c has 13 compilers.\n"
                assert ok(ai, "godbolt_catalog", action="families", limit=2,
                          match="x86-64") == \
                    "family\tcompilers\nclang\t1\ngcc\t2\n" + left(1) + "\nListed 2 of " \
                    "the 3 families of the 3 matching compilers. The list of language c " \
                    "has 13 compilers.\n"
                assert ok(ai, "godbolt_catalog", action="architectures", kind="ticlang") \
                    .endswith("\nListed 1 of the 1 architecture of the 1 matching compiler. "
                              "The list of language c has 13 compilers.\n")

                # limit cuts the rows, and an offset reads on from a byte.
                full = ok(ai, "godbolt_catalog", action="compilers", limit=2)
                assert full.split("\n")[1:5] == [
                    "cg142\tx86-64 gcc 14.2\tdefault\t14.2\tamd64\tstable\tgcc",
                    "cclang19\tx86-64 clang 19.1\tclang\t19.1\tamd64\tstable\tclang,llvm",
                    left(1), "Listed 2 of 13 matching compilers. The list of language c has "
                    "13 compilers."], full
                half = len(full.encode()) // 2
                assert ok(ai, "godbolt_catalog", action="compilers", limit=2,
                          offset=half) == full.encode()[half:].decode()

                # askai stores the lists, so each list was sent once.  Another
                # language is another list.
                assert gets("/api/languages") == 1 and gets("/api/compilers/c") == 1, \
                    ce.seen("GET")
                assert ids(language="c++") == ["cppg142", "cvc19"]
                assert gets("/api/compilers/c++") == 1

                # A list with an error status fails the call, and so does a
                # list that is not a JSON array.  The list of a language
                # without compilers is empty.
                assert refused(ai, "godbolt_catalog", action="compilers",
                               language="broken") == "askai could not get the list of " \
                    "compilers from Compiler Explorer (HTTP 500: No list today)."
                assert refused(ai, "godbolt_catalog", action="kinds", language="odd") == \
                    "Compiler Explorer sent a list that is not valid: the text is not a " \
                    "JSON array."
                assert ok(ai, "godbolt_catalog", action="compilers", language="none") \
                    .endswith("families\nListed 0 of 0 matching compilers. The list of "
                              "language none has 0 compilers.\n")

                # An element with more than 64 family tags, or with a kind of
                # more than 128 bytes, is left out, and a tag that repeats
                # counts once.  An element larger than 4 KiB fails the call,
                # and so do more than 256 different kinds.
                assert ok(ai, "godbolt_catalog", action="families", language="tags") == \
                    "family\tcompilers\na\t1\nb\t1\n" + left(2) + "\nListed 2 of the 2 " \
                    "families of the 1 matching compiler. The list of language tags has 1 " \
                    "compiler.\n"
                assert refused(ai, "godbolt_catalog", action="compilers", language="huge") \
                    == "Compiler Explorer sent a list that is not valid: an element has " \
                    "more than 4 KiB."
                assert refused(ai, "godbolt_catalog", action="kinds", language="keys") == \
                    "Compiler Explorer sent a list that is not valid: the list has more " \
                    "than 256 different kinds."

                # Compilations in the order of the compilers, and then of the
                # argument strings.  The bodies have the 11 filters of the
                # site.  No list of compilers is sent.
                listed = len(ce.seen("GET"))
                text = ok(ai, "godbolt_compile", compilers=["cg142", "cclang19"],
                          args=["-O2", "-O2 -Wall"], source=source)
                expected = "".join(
                    f'=== {cid} "{arg}": exit code 0 ===\n--- asm ---\n{asm}' +
                    (f"--- stderr ---\n{warning}" if "-Wall" in arg else "")
                    for cid in ("cg142", "cclang19") for arg in ("-O2", "-O2 -Wall"))
                assert text == expected + summary(4, 4, 0, 0, 0), text
                bodies = ce.bodies()
                assert [b["options"]["userArguments"] for b in bodies] == \
                    ["-O2", "-O2 -Wall"] * 2, bodies
                for body in bodies:
                    assert body == {"source": source, "lang": "c", "options": {
                        "userArguments": body["options"]["userArguments"],
                        "filters": filters, "compilerOptions": {}}}, body
                assert len(ce.seen("GET")) == listed
                ok(ai, "godbolt_compile", compilers=["cg142"], source=source,
                   filters={"intel": False, "trim": True})
                assert ce.bodies()[-1]["options"]["filters"] == \
                    filters | {"intel": False, "trim": True}
                assert ce.bodies()[-1]["options"]["userArguments"] == ""

                # A failed request is a record, and the call fails only if
                # no compilation has a result.  An exit code is a result.
                records, last = ce_records(ok(ai, "godbolt_compile", source=source,
                                              compilers=["gone", "boom", "cg142"]))
                assert [r[2] for r in records] == [
                    "HTTP 404: Compiler Explorer has no compiler with this id. "
                    "godbolt_catalog lists the ids.",
                    "HTTP 500: Something broke", "exit code 0"] and \
                    last == summary(3, 1, 0, 2, 0), records
                text = refused(ai, "godbolt_compile", compilers=["gone"], source=source)
                assert text.endswith(summary(1, 0, 0, 1, 0)), text
                records, last = ce_records(ok(ai, "godbolt_compile", compilers=["cg142"],
                                              args=["-DFAIL"], source=source))
                assert records[0][2] == "exit code 1" and \
                    "--- stderr ---\n<source>:1:1: error: FAIL is defined\n" in \
                    records[0][3] and last == summary(1, 0, 1, 0, 0), records

                # 429 with Retry-After 1 is sent again after 1 s.  With
                # Retry-After 120 it is a failed request at once.
                start = time.monotonic()
                records, _ = ce_records(ok(ai, "godbolt_compile", compilers=["busy"],
                                           source=source))
                took = time.monotonic() - start
                assert records[0][2] == "exit code 0" and len(ce.bodies("busy")) == 2 \
                    and took >= 0.9, (records, took)
                start = time.monotonic()
                records, _ = ce_records(refused(ai, "godbolt_compile", compilers=["later"],
                                                source=source))
                took = time.monotonic() - start
                assert records[0][2] == "HTTP 429, and Retry-After is 120 s" and \
                    len(ce.bodies("later")) == 1 and took < 2, (records, took)

                # Cut output has a note, and a response larger than 4 MiB is a
                # failed request.  The next compilation still has its record.
                records, _ = ce_records(ok(ai, "godbolt_compile", source=source,
                                           compilers=["trunc", "big", "cg142"]))
                assert records[0][2:] == ("exit code 0", "--- asm ---\nmov eax, 1\n"
                                          "[truncated; too many lines]\n[Compiler "
                                          "Explorer cut this output.]\n") and \
                    records[1][2:] == ("the response is larger than 4 MiB", "") and \
                    records[2][2:] == ("exit code 0", f"--- asm ---\n{asm}"), records

                # The output ends at 14 MiB, also where the room left ends
                # inside a UTF-8 character.  The lengths of the argument strings
                # move the end to each byte of a character.
                for k in range(3):
                    text = ok(ai, "godbolt_compile", compilers=["euro"],
                              args=["-x" + "y" * k + str(i) for i in range(4)],
                              source=source)
                    note = "\n[The output is cut here, because it has reached 14 MiB.]\n"
                    head, _, last = text.rpartition(note)
                    assert len(head.encode()) <= 14 << 20 and \
                        last == summary(4, 4, 0, 0, 0), (k, text[-300:])

                # A line of 3 MB is written in slices, so that no step copies
                # more than 1 MiB.  A response with many label definitions is
                # freed in slices, and a response that is not a JSON object is
                # a failed request.
                records, last = ce_records(ok(ai, "godbolt_compile", source=source,
                                              compilers=["longline", "labels", "array"]))
                assert records[0][2:] == ("exit code 0", "--- stderr ---\n" + "y" * 3_000_000 +
                                          "\n") and \
                    records[1][2:] == ("exit code 0", "") and \
                    records[2][2] == "the response is not a JSON object with an integer " \
                    "member code" and last == summary(3, 2, 0, 1, 0), [r[:3] for r in records]
                copy = [v for k, v in ai.metrics().items()
                        if k.startswith("askai_loop_step_work_max{") and 'unit="copy"' in k]
                assert copy and max(copy) <= 1 << 20, copy

                # A call without offset sends its compilations again.  A call
                # with offset gets the stored responses, and its output is the
                # rest of the same output.
                arguments = {"compilers": ["cg142", "cclang19"], "args": ["-O3"],
                             "source": source}
                sent = len(ce.bodies())
                full = ok(ai, "godbolt_compile", **arguments)
                assert len(ce.bodies()) == sent + 2
                time.sleep(1.1)
                half = len(full.encode()) // 2
                assert ok(ai, "godbolt_compile", offset=half, **arguments) == \
                    full.encode()[half:].decode()
                assert len(ce.bodies()) == sent + 2
                assert ok(ai, "godbolt_compile", **arguments) == full
                assert len(ce.bodies()) == sent + 4

                # godbolt_preprocess reads the kinds of its compilers from the
                # stored list.  MSVC and clang-cl get /TC or /TP and /EP, and
                # stdout has their text.  The other kinds, also those that
                # godbolt-c did not know, get -E and the preprocessed view.
                listed = len(ce.seen("GET"))
                sent = len(ce.bodies())
                records, last = ce_records(ok(ai, "godbolt_preprocess", args=["-O2"],
                                              compilers=["cg142", "cvc19", "cti"],
                                              source=source))
                marker = '# 1 "<source>"\n'
                assert records == [
                    ("cg142", "-O2", "exit code 0", f"--- pp ---\n{marker}{source}"),
                    ("cvc19", "-O2", "exit code 0", f"--- pp ---\n{source}"),
                    ("cti", "-O2", "exit code 0", f"--- pp ---\n{marker}{source}")] and \
                    last == summary(3, 3, 0, 0, 0), records
                bodies = ce.bodies()[sent:]
                producepp = {"producePp": {"filter-headers": False, "clang-format": False},
                             "skipAsm": True}
                assert [(b["options"]["userArguments"], b["options"]["compilerOptions"])
                        for b in bodies] == [("-E -O2", producepp),
                                             ("/TC /EP -O2", {"skipAsm": True}),
                                             ("-E -O2", producepp)], bodies
                assert all(b["options"]["filters"] == dict.fromkeys(filters, False)
                           for b in bodies), bodies
                ok(ai, "godbolt_preprocess", compilers=["cvc19"], language="c++",
                   source=source)
                assert ce.bodies()[-1]["options"]["userArguments"] == "/TP /EP" and \
                    ce.bodies()[-1]["lang"] == "c++"
                assert len(ce.seen("GET")) == listed, ce.seen("GET")[listed:]
                sent = len(ce.bodies())
                assert refused(ai, "godbolt_preprocess", compilers=["cg142", "nope"],
                               source=source) == "These compiler ids are not in the " \
                    "list of language c: nope. godbolt_catalog lists the ids."
                assert len(ce.bodies()) == sent

                # godbolt_macros sends no request for MSVC, and its source is
                # optional.
                records, last = ce_records(ok(ai, "godbolt_macros",
                                              compilers=["cg142", "cvc19", "cclangcl19"]))
                assert records == [
                    ("cg142", "", "exit code 0", "--- macros ---\n#define __GNUC__ 14\n"),
                    ("cvc19", "", "not sent, because MSVC cannot output its predefined "
                     "macros", ""),
                    ("cclangcl19", "", "exit code 0",
                     "--- macros ---\n#define _MSC_VER 1940\n")] and \
                    last == summary(3, 2, 0, 0, 1), records
                bodies = ce.bodies()[sent:]
                assert [(b["options"]["userArguments"], b["source"]) for b in bodies] == \
                    [("-E -dM", "\n"), ("/TC /E /clang:-dM", "\n")], bodies

                # godbolt_targets preprocesses the probe, and the record has the
                # facts that the macros show.  With headers, the source makes
                # the compiler include the target headers first.
                def facts(macros, **values):
                    lines = [f"{name}: {values.get(name, 'unknown')}"
                             for name in ("architecture", "os", "os_family", "data_model",
                                          "pointer_bits", "endianness", "libc")]
                    versions = " ".join(f"{name}={macros[name]}" for name in (
                        "__clang_major__", "__GNUC__", "__GNUC_MINOR__", "_MSC_VER",
                        "__STDC_VERSION__", "__cplusplus") if name in macros)
                    return "".join(line + "\n" for line in lines) + \
                        f"versions: {versions}\n"
                gnu = facts(CE_PROBE_GNU, architecture="x86_64 (from __x86_64__)",
                            os="linux (from __linux__)", os_family="linux (from __linux__)",
                            data_model="lp64 (from __LP64__, __SIZEOF_INT__, "
                            "__SIZEOF_LONG__, __SIZEOF_POINTER__, __CHAR_BIT__)",
                            pointer_bits="64 (from __SIZEOF_POINTER__, __CHAR_BIT__)",
                            endianness="little (from __BYTE_ORDER__, "
                            "__ORDER_LITTLE_ENDIAN__)", libc="glibc (from __GLIBC__)")
                msvc = facts(CE_PROBE_MSVC, architecture="x86_64 (from _M_X64)",
                             os="windows (from _WIN64)", os_family="windows (from _WIN64)",
                             data_model="llp64 (from _WIN64)",
                             pointer_bits="64 (from _WIN64)")
                note = "The facts come from predefined macros. data_model describes " \
                    "only the sizes of int, long and pointers.\n"
                sent = len(ce.bodies())
                text = ok(ai, "godbolt_targets", compilers=["cg142", "cvc19", "cti"],
                          args=["-m64"])
                assert text.endswith(note + summary(3, 3, 0, 0, 0)), text
                records, _ = ce_records(text.removesuffix(note + summary(3, 3, 0, 0, 0))
                                        + summary(3, 3, 0, 0, 0))
                assert records == [
                    ("cg142", "-m64", "exit code 0", gnu),
                    ("cvc19", "-m64", "exit code 0", msvc),
                    ("cti", "-m64", "the probe output is not valid: the output has no "
                     "complete block", "--- pp ---\nGBPROBE_BEGIN_1\n")], records
                bodies = ce.bodies()[sent:]
                assert [b["options"]["userArguments"] for b in bodies] == \
                    ["-E -m64", "/TC /EP -m64", "-E -m64"], bodies
                assert bodies[0]["source"].startswith("/* Preprocess only: ") and \
                    len(bodies[0]["source"].encode()) == 14865, bodies[0]["source"][:80]
                text = ok(ai, "godbolt_targets", compilers=["cg142"], headers=True)
                assert ce.bodies()[-1]["source"] == \
                    "#define GODBOLT_PROBE_HEADERS 1\n" + bodies[0]["source"]
                assert gnu + "headers: limits.h included, features.h included, " \
                    "TargetConditionals.h included, _mingw.h included\n" in text, text
                # A value of more than 256 bytes makes the probe output not
                # valid.
                records, _ = ce_records(ok(ai, "godbolt_targets", compilers=["longvalue"],
                                           language="probe").removesuffix(note))
                assert records[0][2] == "the probe output is not valid: a name or a value " \
                    "of a record has more than 256 bytes", records

                # With share, the first line is a short link to the state of the
                # call, which Compiler Explorer stores.  A call with offset sends
                # no request, and a call without offset sends the compilations
                # again, but the same state gets the stored link.
                shared = {"compilers": ["cg142"], "args": ["-O2"], "source": source,
                          "share": True}
                text = ok(ai, "godbolt_compile", **shared)
                link, _, rest = text.partition("\n")
                assert re.fullmatch("Link: " + re.escape(ce.url()) + "/z/[0-9a-f]{10}",
                                    link), text
                (_, _, _, headers, body), = ce.seen("POST", "/api/shortener")
                state = {"sessions": [{"id": 1, "language": "c", "source": source,
                                       "compilers": [{"id": "cg142", "options": "-O2",
                                                      "filters": filters, "libs": []}]}]}
                assert headers["Content-Type"] == "application/json" and \
                    json.loads(body) == state, (dict(headers), body)
                records, last = ce_records(rest)
                assert [r[2] for r in records] == ["exit code 0"] and \
                    last == summary(1, 1, 0, 0, 0), rest
                posts = len(ce.seen("POST"))
                time.sleep(1.1)
                half = len(text.encode()) // 2
                assert ok(ai, "godbolt_compile", offset=half, **shared) == \
                    text.encode()[half:].decode()
                assert len(ce.seen("POST")) == posts
                assert ok(ai, "godbolt_compile", **shared) == text
                assert len(ce.seen("POST")) == posts + 1 and \
                    len(ce.seen("POST", "/api/shortener")) == 1

                # The state of godbolt_macros has the flags of the recipes, and
                # no compiler that the call does not send.  A link that fails
                # is a line with the reason, and the compilations go on.
                text = ok(ai, "godbolt_macros", compilers=["cg142", "cvc19"], share=True)
                assert text.startswith(f"Link: {ce.url()}/z/"), text
                assert json.loads(ce.seen("POST", "/api/shortener")[-1][4]) == \
                    {"sessions": [{"id": 1, "language": "c", "source": "\n", "compilers": [
                        {"id": "cg142", "options": "-E -dM",
                         "filters": dict.fromkeys(filters, False), "libs": []}]}]}
                # A call that sends no compilation gets no link, and Compiler
                # Explorer stores nothing.
                shortened = len(ce.seen("POST", "/api/shortener"))
                text = refused(ai, "godbolt_macros", compilers=["cvc19"], source="int x;\n",
                               share=True)
                assert text.startswith("Link: none. The call sends no compilation.\n") and \
                    len(ce.seen("POST", "/api/shortener")) == shortened, text
                # The state of a call with long argument strings is written in
                # more than one step, so that no step escapes more than 64 KiB.
                ok(ai, "godbolt_compile", compilers=["cg142", "cclang19", "cti", "cgtrunk"],
                   args=[f"-D{i}" + "x" * 8188 for i in range(8)], source=source, share=True)
                escape = [v for k, v in ai.metrics().items()
                          if k.startswith("askai_loop_step_work_max{") and 'unit="escape"' in k]
                assert escape and max(escape) <= 64 << 10, escape
                text = ok(ai, "godbolt_compile", compilers=["cg142"], source="// NOSHORT\n",
                          share=True)
                link, _, rest = text.partition("\n")
                assert link == "Link: none. The request for a short link failed " \
                    "(HTTP 500: Storage is broken)." and \
                    ce_records(rest)[0][0][2] == "exit code 0", text

                # Refused arguments send no request.
                sent = len(ce.seen())
                for arguments, why in (
                        ({"colour": 1}, "The tool has no argument colour."),
                        ({"compilers": [f"c{i}" for i in range(17)]},
                         "compilers must be an array of 1 to 16 different compiler "
                         "ids. A compiler id has 1 to 128 bytes of A-Z, a-z, 0-9, +, "
                         "-, . and _, and it is not . or .."),
                        ({"compilers": ["cg142", "cg142"]}, "compilers must be"),
                        ({"compilers": ["a/b"]}, "compilers must be"),
                        ({"compilers": [".."]}, "compilers must be"),
                        ({"compilers": ["a", "b", "c", "d"], "args": [""] * 9},
                         "args must be an array of 1 to 8 strings. Each string has at "
                         "most 8192 bytes and no NUL byte."),
                        ({"compilers": ["a", "b", "c", "d", "e"], "args": [""] * 7},
                         "The number of compilers times the number of argument strings "
                         "must be at most 32."),
                        ({"args": ["x" * 8193]}, "args must be"),
                        ({"source": "a\0b"}, "source must be a string of at most 1 MiB "
                         "without a NUL byte."),
                        ({"language": "c?"}, "language must have 1 to 64 bytes of a-z, "
                         "0-9, +, - and _."),
                        ({"filters": {"colour": True}}, "filters must be an object whose "
                         "members are among intel, demangle, directives, commentOnly, "
                         "labels, libraryCode and trim. Each member is true or false."),
                        ({"filters": {"intel": 1}}, "filters must be"),
                        ({"source": None}, "The argument source is required."),
                        ({"offset": -1}, "The offset argument must be a whole number "
                         "of bytes, from 0.")):
                    arguments = {"compilers": ["cg142"], "source": source} | arguments
                    arguments = {k: v for k, v in arguments.items() if v is not None}
                    text = refused(ai, "godbolt_compile", **arguments)
                    assert text.startswith(why), (arguments, text)
                for arguments, why in (
                        ({}, "The argument action is required."),
                        ({"action": "standards"}, "action must be languages, compilers, "
                         "kinds, families or architectures."),
                        ({"action": "compilers", "limit": 0}, "limit must be a whole "
                         "number from 1 to 2000."),
                        ({"action": "compilers", "limit": 2001}, "limit must be"),
                        ({"action": "compilers", "match": " ".join("abcdefghi")},
                         "match must be a string of at most 256 bytes with at most 8 "
                         "words."),
                        ({"action": "compilers", "kind": ""}, "kind must be a string of "
                         "1 to 128 bytes without a NUL byte."),
                        ({"action": "compilers", "source": "x"}, "The tool has no "
                         "argument source.")):
                    assert refused(ai, "godbolt_catalog", **arguments).startswith(why), \
                        arguments
                for name, arguments, why in (
                        ("godbolt_preprocess", {"compilers": ["cg142"]},
                         "The argument source is required."),
                        ("godbolt_preprocess", {"compilers": ["cg142"], "source": "x",
                                                "filters": {}},
                         "The tool has no argument filters."),
                        ("godbolt_macros", {"compilers": ["cg142"], "headers": True},
                         "The tool has no argument headers."),
                        ("godbolt_targets", {"compilers": ["cg142"], "source": "x"},
                         "The tool has no argument source."),
                        ("godbolt_targets", {"compilers": ["cg142"], "headers": 1},
                         "headers must be true or false.")):
                    assert refused(ai, name, **arguments) == why, (name, arguments)
                assert len(ce.seen()) == sent, ce.seen()[sent:]

                # A notification ends a call whose compilation is under way, at
                # once, and askai goes on working.
                results = []
                thread = threading.Thread(target=lambda: results.append(ai.rpc(
                    "tools/call", {"name": "godbolt_compile", "arguments": {
                        "compilers": ["slow"], "source": source}}, rpc_id=7)))
                start = time.monotonic()
                thread.start()
                wait_for(lambda: ce.bodies("slow"))
                assert ai.rpc("notifications/cancelled", {"requestId": 7},
                              rpc_id=None)[0] == 202
                thread.join(5)
                assert time.monotonic() - start < 1.5, "the call waited on"
                assert results and not (results[0][2] or {}).get("result"), results
                assert ai.rpc("ping")[0] == 200

            # The option share state makes links that have the state in
            # base64.  Compiler Explorer stores nothing, so the tools are
            # read-only.  A link longer than 8192 bytes is none.
            write({"url": ce.url(), "share": "state"})
            with Askai(binary, "-c", path) as ai:
                tools = ai.rpc("tools/list")[2]["result"]["tools"]
                assert "share" in tools[2]["inputSchema"]["properties"] and \
                    tools[2]["annotations"]["readOnlyHint"] and \
                    "The link contains the source" in tools[2]["description"], tools[2]
                shortened = len(ce.seen("POST", "/api/shortener"))
                text = ok(ai, "godbolt_compile", **shared)
                link, _, rest = text.partition("\n")
                prefix = f"Link: {ce.url()}/clientstate/"
                assert link.startswith(prefix) and json.loads(base64.b64decode(
                    link.removeprefix(prefix), validate=True)) == state, link
                assert ce_records(rest)[0][0][2] == "exit code 0", rest
                text = ok(ai, "godbolt_compile", compilers=["cg142"], share=True,
                          source="int x;\n" * 1000)
                link, _, rest = text.partition("\n")
                assert link == "Link: none. The link would be longer than 8192 bytes." \
                    and ce_records(rest)[0][0][2] == "exit code 0", text
                assert len(ce.seen("POST", "/api/shortener")) == shortened

            # The option share off removes the argument share, and a call with
            # it is refused.
            write({"url": ce.url(), "share": "off"})
            with Askai(binary, "-c", path) as ai:
                tools = ai.rpc("tools/list")[2]["result"]["tools"]
                for tool in tools[1:]:
                    assert "share" not in tool["inputSchema"]["properties"] and \
                        tool["annotations"]["readOnlyHint"] and \
                        "share" not in tool["description"], tool
                sent = len(ce.seen())
                assert refused(ai, "godbolt_compile", **shared) == "Links are turned " \
                    "off on this server, so the tool has no argument share."
                assert len(ce.seen()) == sent

        # The plugin refuses options that it does not know, and a url that is
        # not an http or https URL without a user name, a query or a
        # fragment.
        with tempfile.TemporaryDirectory() as d:
            path = f"{os.path.realpath(d)}/askai.json"
            for options, why in (
                    ({"colour": 1}, "the member colour is not an option"),
                    ({"url": "ftp://x"}, "url must be an http or https URL of at most "
                     "255 bytes, without a user name, a password, a query or a "
                     "fragment"),
                    ({"url": "http://user@x"}, "url must be"),
                    ({"url": "http://x/?q"}, "url must be"),
                    ({"url": "http:///"}, "url must be"),
                    ({"url": "http://" + "x" * 249}, "url must be"),
                    ({"url": 5}, "url must be"),
                    ({"share": "maybe"}, "share must be short, state or off"),
                    ({"share": True}, "share must be short, state or off")):
                with open(path, "w") as f:
                    json.dump(grouped({"url": up.url(), "plugins": [
                                {"path": godbolt, "options": options}]}), f)
                os.chmod(path, 0o600)
                run = subprocess.run([binary, "-l", "127.0.0.1:0", "-c", path],
                                     stdin=subprocess.DEVNULL, capture_output=True,
                                     text=True, timeout=5, check=False)
                assert run.returncode == 2 and \
                    f"askai: {godbolt}: options: {why}" in run.stderr, \
                    (options, run.returncode, run.stderr)
    finally:
        ce.close()
    print("godbolt: tools listed with the configured URL; catalog of languages and "
          "compilers with filters, counts, limit and offset, lists stored, "
          "malformed and oversized elements; compilations in order with the site's "
          "filters, failed requests as records, 429 sent again, cut and oversized "
          "responses, the end of the output at 14 MiB, a long line in slices, "
          "large and odd responses, stored responses for a call with offset, "
          "refused arguments and options, cancellation; preprocessing, predefined "
          "macros and target facts with the flags of each driver; short links, "
          "links to the state and no links: PASS")


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
        wait_for(lambda: len(ai.listed("upstream requests:") or ()) == 6)
        counts = ai.listed("upstream requests:")
        assert counts[-2:] == ["aborted    1", "preempted  1"], counts
    finally:
        up.reply = hold
    print("classes: background work from test_task's prompt, a slot kept for "
          "interactive work, preemption with one slot, once: PASS")


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else "./askai"
    # The test plugin is built beside askai (tests/Makefile).
    plugin = sys.argv[2] if len(sys.argv) > 2 else \
        os.path.join(os.path.dirname(binary), "test_plugin.so")
    godbolt = sys.argv[3] if len(sys.argv) > 3 else \
        os.path.join(os.path.dirname(binary), "godbolt.so")
    up = Upstream()
    try:
        for check in (check_legacy_session, check_protocols, check_upstream_errors,
                      check_modern, check_mcp_tasks, check_client_jobs, check_http,
                      check_large_bodies,
                      check_sse,
                      check_answers, check_dense, check_connections,
                      check_cancellation, check_limits, check_loops, check_turn_order,
                      check_isolation,
                      lambda binary, up: check_configuration(binary, up, godbolt),
                      check_allow, check_identities,
                      check_reload, check_logs, check_unix, check_tls, check_metrics,
                      check_memory, check_lookups, check_jobs, check_streaming,
                      check_scheduler, check_breaker, check_slots, check_progress,
                      check_man, check_git, check_turns, check_notes, check_turn_apis,
                      check_tasks,
                      lambda binary, up: check_plugin(binary, up, plugin),
                      lambda binary, up: check_instances(binary, up, plugin),
                      lambda binary, up: check_misuse(binary, up, plugin),
                      lambda binary, up: check_web(binary, up, plugin),
                      lambda binary, up: check_godbolt(binary, up, godbolt),
                      check_classes,
                      check_shutdown,
                      check_watchdog, check_stalled_stderr):
            check(binary, up)
    finally:
        up.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
