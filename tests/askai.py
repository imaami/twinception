#!/usr/bin/env python3
"""End-to-end checks for the askai MCP server against fake AI endpoints."""

import fcntl
import http.client
import json
import os
import re
import resource
import select
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

LEGACY = "2025-11-25"
MODERN = "2026-07-28"
KEY_VARS = ("ASKAI_TOKEN", "OPENAI_API_KEY", "ANTHROPIC_API_KEY",
            "GEMINI_API_KEY", "GOOGLE_API_KEY")

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


class Upstream:
    """Fake AI endpoint; reply(request) gives (status, payload[, delay])."""

    def __init__(self):
        self.requests = []
        self.reply = lambda request: (200, ANSWERS["openai"])
        self.arrived = threading.Event()
        self.cancelled = threading.Event()
        self.lock = threading.Lock()
        upstream = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, fmt, *args):
                pass

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
                    upstream.cancelled.set()
                    return
                wire = payload if isinstance(payload, bytes) else \
                    json.dumps(payload).encode()
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(wire)))
                self.end_headers()
                self.wfile.write(wire)

        self.server = QuietServer(("127.0.0.1", 0), Handler)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    @staticmethod
    def wait_for_hangup(conn, seconds):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            readable, _, _ = select.select([conn], [], [], 0.02)
            if readable and conn.recv(1, socket.MSG_PEEK) == b"":
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
    With nofile, it starts with that descriptor limit, soft and hard; with
    preload, with that library preloaded; with ignore, with those signals
    ignored."""

    def __init__(self, binary, *args, env=None, nofile=None, preload=None,
                 ignore=()):
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
            for sig in ignore:
                signal.signal(sig, signal.SIG_IGN)

        self.proc = subprocess.Popen(
            [binary, "-l", "127.0.0.1:0", *args], env=child_env,
            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE, text=True,
            preexec_fn=setup if nofile or ignore else None)
        line = self.proc.stderr.readline()
        match = re.search(r"http://([\d.]+):(\d+)/mcp$", line.strip())
        if not match:
            self.proc.kill()
            raise AssertionError("askai did not start: " + line +
                                 self.proc.stderr.read())
        self.host = match.group(1)
        self.port = int(match.group(2))
        self.log = []
        threading.Thread(target=self.drain, daemon=True).start()

    def drain(self):
        for line in self.proc.stderr:
            self.log.append(line)

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
        if "name" in params:
            all_headers["Mcp-Name"] = params["name"]
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
        assert request["body"] == {"model": "m", "messages": [
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
    expect = {
        "openai": ("/v1/chat/completions", "authorization", "Bearer secret", {
            "model": "m", "max_tokens": 77, "temperature": 0.25, "top_p": 0.5,
            "messages": [{"role": "system", "content": "SYS"},
                         {"role": "user", "content": "Q"}]}),
        "responses": ("/v1/responses", "authorization", "Bearer secret", {
            "model": "m", "input": "Q", "instructions": "SYS", "store": False,
            "max_output_tokens": 77, "temperature": 0.25, "top_p": 0.5}),
        "anthropic": ("/v1/messages", "x-api-key", "secret", {
            "model": "m", "max_tokens": 77, "temperature": 0.25, "top_p": 0.5,
            "system": "SYS", "messages": [{"role": "user", "content": "Q"}]}),
        "gemini": ("/v1beta/models/m:generateContent", "x-goog-api-key", "secret", {
            "contents": [{"role": "user", "parts": [{"text": "Q"}]}],
            "systemInstruction": {"parts": [{"text": "SYS"}]},
            "generationConfig": {"maxOutputTokens": 77, "temperature": 0.25},
            "top_p": 0.5}),
    }
    for protocol, (path, auth, value, body) in expect.items():
        with Askai(binary, "-p", protocol, "-u", up.url(path), "-m", "m",
                   "-k", "TEST_KEY", "-s", "SYS", "-n", "77", "-t", "0.25",
                   "-x", '{"top_p": 0.5, "store": null}',
                   env={"TEST_KEY": "secret"}) as ai:
            up.reply = lambda request, p=protocol: (200, ANSWERS[p])
            assert text_of(ai.ask()) == "A", protocol
            request = up.last()
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

    with Askai(binary, "-p", "anthropic", "-u", up.url("/v1/messages")) as ai:
        up.reply = lambda request: (200, ANSWERS["anthropic"])
        ai.ask()
        assert up.last()["body"]["max_tokens"] == 4096
        assert "model" not in up.last()["body"]

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
        assert result["capabilities"] == {"tools": {}}
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
    with Askai(binary, "-u", up.url()) as ai:
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

    # Clients may hold all but 64 descriptors, a tool call counting one more
    # for its upstream connection.  New clients and calls beyond that make
    # room by closing the least recently active idle connection, so idle
    # clients cannot leave tool calls without a descriptor.
    up.reply = lambda request: (200, ANSWERS["openai"], 0.5)
    with Askai(binary, "-u", up.url(), nofile=192) as ai:
        held = [held_ping(ai.port, i) for i in range(128)]
        held.append(held_ping(ai.port, 128))
        assert closed_by_peer(held[0])
        call = rpc_request(1, {"name": "ask_ai", "arguments": {"prompt": "Q"}},
                           "tools/call")
        callers = held[-60:]
        for sock in callers:
            sock.sendall(call)
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

    with Askai(binary, "-u", up.url()) as ai:
        _, headers, _ = ai.rpc("initialize", {"protocolVersion": LEGACY})
        session = {"Mcp-Session-Id": headers["mcp-session-id"],
                   "MCP-Protocol-Version": LEGACY}
        up.reply = slow

        def call(results):
            results.append(ai.rpc("tools/call", {"name": "ask_ai", "arguments": {
                "prompt": "SLOW"}}, rpc_id=42, headers=session))

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

        # A legacy client may cancel by notification.  On an event stream
        # the stream then ends without an event; a JSON POST gets a 202.
        for accept, expected in (("application/json, text/event-stream", 200),
                                 ("application/json", 202)):
            up.arrived.clear()
            up.cancelled.clear()
            results = []
            session["Accept"] = accept
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
        del session["Accept"]

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

    print("cancellation by notification, hang-up and half-close: PASS")


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

    # SIGUSR1 prints the loop's statistics since the previous one.
    with Askai(binary, "-u", up.url()) as ai:
        assert ai.rpc("ping")[0] == 200
        ai.proc.send_signal(signal.SIGUSR1)
        for _ in range(100):
            if any(line.startswith("askai: stats work ") for line in ai.log):
                break
            time.sleep(0.01)
        stats = [line.split()[2] for line in ai.log if line.startswith("askai: stats ")]
        assert stats == ["turn", "interactive", "bulk", "timer_late", "work"], ai.log
        assert re.search(r"^askai: stats turn n=[1-9]\d* ", "".join(ai.log), re.M)
        # The most work of each unit a step did, against its bound.
        units = {k: tuple(map(int, v.split("/")))
                 for k, v in re.findall(r" (\w+)=(\d+/\d+)", ai.log[-1])}
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


def check_large_bodies(binary, up):
    """Bodies are parsed, and client strings escaped into the upstream request,
    a slice at a time: long text with escapes and multi-byte characters, where
    any slice may end, arrives whole, in every protocol's order of fields."""
    prompt = 'Ünïcödé "quoted"\n\t\u20ac\U0001f600 \\ \x01 end. ' * 30000
    system = "s\u00e9\x02" * 20000
    for protocol in ("openai", "responses", "anthropic", "gemini"):
        path = "/v1beta/models/m:generateContent" if protocol == "gemini" else "/x"
        with Askai(binary, "-p", protocol, "-u", up.url(path), "-m", "m",
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
                "responses": lambda b: (b["input"], b["instructions"]),
                "anthropic": lambda b: (b["messages"][0]["content"], b["system"]),
                "gemini": lambda b: (b["contents"][0]["parts"][0]["text"],
                                     b["systemInstruction"]["parts"][0]["text"]),
            }[protocol](sent)
            assert texts == (prompt, system), protocol
    up.reply = lambda request: (200, ANSWERS["openai"])

    with Askai(binary, "-u", up.url(), "-s", "SYS", "-x", '{"messages": null}') as ai:
        assert text_of(ai.ask("Q" * 100_000)) == "A"
        assert up.last()["body"] == {}
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
    bound), and how many bulk steps ran."""
    ai.log.clear()
    ai.proc.send_signal(signal.SIGUSR1)
    for _ in range(200):
        if any(line.startswith("askai: stats work ") for line in ai.log):
            break
        time.sleep(0.01)
    line = next(line for line in ai.log if line.startswith("askai: stats work "))
    steps = next(line for line in ai.log if line.startswith("askai: stats bulk "))
    units = {k: tuple(map(int, v.split("/")))
             for k, v in re.findall(r" (\w+)=(\d+/\d+)", line)}
    return units, int(re.search(r" n=(\d+)", steps).group(1))


def check_answers(binary, up):
    """An upstream answer is parsed a slice at a time, however many values it
    holds, and its reply goes out a piece at a time as the client reads it,
    however large: a response on its own may take the pending output past
    --max-output.  Reading the answer's text copies it whole, which the work
    line shows once that passes a step's bound."""
    call = {"name": "ask_ai", "arguments": {"prompt": "Q"}}
    for args, size in ((("--output-pause", "16K", "--max-output", "64K"), 100_000),
                       ((), 1_100_000)):
        text = "x\n\"" * (size // 3)
        up.reply = lambda request, t=text: (200, {"choices": [{
            "message": {"content": t}, "finish_reason": "stop"}]})
        with Askai(binary, "-u", up.url(), *args) as ai:
            work(ai)
            for accept in ("application/json", "application/json, text/event-stream"):
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
            assert (units["copy"][0] > units["copy"][1]) == (size > 1 << 20), units

    values = 200_000
    up.reply = lambda request: (200, {"choices": [{
        "message": {"content": "A"}, "finish_reason": "stop",
        "logprobs": {"content": [0] * values}}]})
    with Askai(binary, "-u", up.url()) as ai:
        work(ai)
        assert text_of(ai.ask()) == "A"
        units, bulk = work(ai)
        assert all(most <= bound for most, bound in units.values()), units
        assert bulk >= values // units["values"][1], (bulk, units)
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
    with Askai(binary, "-u", up.url(), "--max-values", "1000000", "--max-body", "4M") as ai:
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

        # Once the loop is quiet, the memory those took goes back; ASan's
        # and TSan's allocators keep it.
        def rss():
            with open(f"/proc/{ai.proc.pid}/status") as f:
                return int(re.search(r"VmRSS:\s+(\d+)", f.read()).group(1))
        with open(f"/proc/{ai.proc.pid}/exe", "rb") as f:
            exe = f.read()
            sanitized = b"__asan_init" in exe or b"__tsan_init" in exe
        for _ in range(0 if sanitized else 80):
            if rss() < start_rss + 16384:
                break
            time.sleep(0.1)
        assert sanitized or rss() < start_rss + 16384, (start_rss, rss())

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
    with Askai(binary, "-u", up.url(), "--max-client-conns", "0") as ai:
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

    print("failure isolation: an unwatchable upstream socket, accept errors, "
          "a slow teardown: PASS")


def check_configuration(binary, up):
    def fails(*args, env=None, message):
        child_env = {k: v for k, v in os.environ.items() if k not in KEY_VARS}
        child_env.update(env or {})
        run = subprocess.run([binary, "-l", "127.0.0.1:0", *args], env=child_env,
                             stdin=subprocess.DEVNULL, capture_output=True,
                             text=True, timeout=5, check=False)
        assert run.returncode == 2 and message in run.stderr, (args, run.stderr)

    fails("-p", "anthropic", "-m", "m", message="set ANTHROPIC_API_KEY")
    fails("-p", "gemini", env={"GEMINI_API_KEY": "k"}, message="--model is required")
    fails("-u", up.url(), "-k", "UNSET_VAR", message="UNSET_VAR is not set")
    fails("-p", "cohere", message="unknown protocol")
    fails("-u", "ftp://x", message="not an http(s) URL")
    fails("-t", "warm", message="invalid temperature")
    fails("-t", "-1", message="invalid temperature")
    fails("-n", "0", message="invalid max token count")
    fails("-x", "[1]", message="needs a JSON object")
    fails("-H", "Bad Header: x", message="invalid header line")
    fails("--tool-name", "ask ai", message="tool names")
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

    with Askai(binary, "-p", "gemini", "-m", "m",
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

    print("configuration and key handling: PASS")


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
        for series, least in (('askai_loop_turn_seconds_count{loop="0"}', 1),
                              ('askai_loop_step_seconds_count{loop="0",'
                               'kind="interactive"}', 1),
                              ('askai_loop_step_work_max{loop="0",unit="reqs"}', 1),
                              ('askai_request_seconds_count{method="ping"}', 1),
                              ('askai_request_seconds_count{method="tools/call"}', 1),
                              ('askai_request_seconds_count{method="other"}', 1),
                              ("askai_upstream_wait_seconds_count", 1),
                              ("askai_upstream_run_seconds_count", 1),
                              ("askai_resident_bytes", 1 << 20),
                              ("askai_open_fds", 4)):
            assert values[series] >= least, (series, values.get(series))
        assert values["askai_max_fds"] >= values["askai_open_fds"]
        # The descriptor table was grown before threads shared it: growing it
        # after makes the kernel wait for an RCU grace period, in a step.
        with open(f"/proc/{ai.proc.pid}/status") as f:
            size = int(re.search(r"FDSize:\s+(\d+)", f.read()).group(1))
        assert size >= min(values["askai_max_fds"], 65536), size
        assert values["askai_log_dropped_total"] == 0
        assert values['askai_upstream_lookups_total{result="ok"}'] == 0
        # Rendering one is a request too.
        assert ai.metrics()['askai_request_seconds_count{method="metrics"}'] >= 1
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

    with Askai(binary, "-u", "http://nonexistent.invalid:1/v1/chat/completions") as ai:
        result = ai.ask()
        assert result["isError"] and \
            "cannot look up nonexistent.invalid: " in text_of(result), result
        assert ai.metrics()['askai_upstream_lookups_total{result="failed"}'] == 1

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
        closes = values['askai_loop_step_work_max{loop="0",unit="closes"}']
        assert 1 < closes <= 32, closes
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


def check_watchdog(binary, up):
    """The watchdog reports a turn that goes on too long, once; a shutdown
    signal ends a stuck loop's process at once, as does a second signal."""
    with Askai(binary, "-u", up.url(), "--test-stall", "300") as ai:
        conn = ai.connect()
        assert ai.rpc("ping", conn=conn)[0] == 200
        ai.proc.send_signal(signal.SIGUSR2)
        time.sleep(0.05)
        start = time.monotonic()
        assert ai.rpc("ping", conn=conn)[0] == 200
        assert time.monotonic() - start > 0.15, "the loop did not stall"
        conn.close()
        ai.wait_log(r"watchdog: the event loop has been in one turn for \d+")
        values = ai.metrics()
        assert values["askai_watchdog_reports_total"] >= 1, values
        assert values['askai_loop_turn_seconds_bucket{loop="0",le="0.25"}'] < \
            values['askai_loop_turn_seconds_count{loop="0"}']

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


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else "./askai"
    up = Upstream()
    try:
        for check in (check_legacy_session, check_protocols, check_upstream_errors,
                      check_modern, check_http, check_large_bodies, check_sse,
                      check_answers, check_dense, check_connections,
                      check_cancellation, check_limits, check_turn_order,
                      check_isolation, check_configuration, check_metrics,
                      check_lookups, check_shutdown, check_watchdog,
                      check_stalled_stderr):
            check(binary, up)
    finally:
        up.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
