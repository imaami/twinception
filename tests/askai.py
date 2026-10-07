#!/usr/bin/env python3
"""End-to-end checks for the askai MCP server against fake AI endpoints."""

import http.client
import json
import os
import re
import select
import signal
import socket
import subprocess
import sys
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
    """An askai process on an ephemeral port; stop() expects a clean exit."""

    def __init__(self, binary, *args, env=None):
        child_env = {k: v for k, v in os.environ.items() if k not in KEY_VARS}
        child_env.update(env or {})
        self.proc = subprocess.Popen(
            [binary, "-l", "127.0.0.1:0", *args], env=child_env,
            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE, text=True)
        line = self.proc.stderr.readline()
        match = re.search(r"http://127\.0\.0\.1:(\d+)/mcp$", line.strip())
        if not match:
            self.proc.kill()
            raise AssertionError("askai did not start: " + line +
                                 self.proc.stderr.read())
        self.port = int(match.group(1))
        self.log = []
        threading.Thread(target=self.drain, daemon=True).start()

    def drain(self):
        for line in self.proc.stderr:
            self.log.append(line)

    def connect(self):
        return http.client.HTTPConnection("127.0.0.1", self.port, timeout=10)

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
        return (response.status,
                {k.lower(): v for k, v in response.getheaders()},
                json.loads(raw) if raw else None)

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
        assert status_of(b"POST /mcp HTTP/1.1\r\n"
                         b"Transfer-Encoding: chunked\r\n\r\n") == 411
        assert status_of(b"POST /mcp HTTP/1.1\r\nMcp-Method: a\r\nMcp-Method: b\r\n"
                         b"Content-Length: 2\r\n\r\n{}") == 400
        reply = ai.raw(b"POST /mcp HTTP/1.0\r\nContent-Type: application/json\r\n" +
                       length + b"\r\n" + ping % 5)
        assert b"Connection: close" in reply and b'"id":5' in reply

    with Askai(binary, "-u", up.url(), env={"ASKAI_TOKEN": "s3cret"}) as ai:
        status, headers, _ = ai.rpc("ping")
        assert status == 401 and headers["www-authenticate"] == "Bearer"
        assert ai.rpc("ping", headers={"Authorization": "Bearer nope"})[0] == 401
        assert ai.rpc("ping", headers={"Authorization": "Bearer s3cret"})[0] == 200
        conn = ai.connect()
        conn.request("OPTIONS", "/mcp", headers={"Origin": "http://localhost"})
        assert conn.getresponse().status == 204
        conn.close()

    print("HTTP framing, CORS, Origin and token checks: PASS")


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

        up.arrived.clear()
        results = []
        thread = threading.Thread(target=call, args=(results,))
        thread.start()
        assert up.arrived.wait(5)
        status, _, _ = ai.rpc("notifications/cancelled",
                              {"requestId": 42, "reason": "stop"},
                              rpc_id=None, headers=session)
        assert status == 202
        thread.join(5)
        assert results[0][0] == 202 and results[0][2] is None
        assert up.cancelled.wait(2), "upstream request was not cancelled"

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
    up.reply = lambda request: (200, ANSWERS["openai"])

    print("cancellation by notification and by hang-up: PASS")


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


def main():
    binary = sys.argv[1] if len(sys.argv) > 1 else "./askai"
    up = Upstream()
    try:
        for check in (check_legacy_session, check_protocols, check_upstream_errors,
                      check_modern, check_http, check_cancellation,
                      check_configuration):
            check(binary, up)
    finally:
        up.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
