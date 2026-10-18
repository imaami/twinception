#!/usr/bin/env python3
"""CLI signal, worker ownership and bounded-output integration checks on Linux."""

import errno
import fcntl
import os
import pty
import select
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import threading
import time

from cli import Peer, command, sse_wire


def response(text):
    return 200, "application/json", {"result": {"content": [{"type": "text", "text": text}]}}


def finish(proc):
    if proc.poll() is None:
        proc.kill()
    proc.wait(timeout=5)
    for stream in (proc.stdin, proc.stdout, proc.stderr):
        if stream:
            stream.close()


def await_condition(proc, condition, description, timeout=5):
    deadline = time.monotonic() + timeout
    while not condition():
        assert proc.poll() is None, (description, "CLI exited", proc.returncode)
        assert time.monotonic() < deadline, description
        time.sleep(0.005)


def holds_file(proc, path):
    try:
        names = os.listdir(f"/proc/{proc.pid}/fd")
    except FileNotFoundError:
        return False
    for name in names:
        try:
            if os.readlink(f"/proc/{proc.pid}/fd/{name}") == path:
                return True
        except FileNotFoundError:
            pass
    return False


def check_sticky_termination(cli):
    arrived = threading.Event()
    cancelling = threading.Event()
    release = threading.Event()

    def reply(request):
        arrived.set()
        release.wait(10)
        return response("late answer")

    def methods(request):
        if request.get("method") == "notifications/cancelled":
            cancelling.set()
            release.wait(10)
            return 202, "application/json", b""
        return None

    with Peer(reply, methods) as peer:
        master, slave = pty.openpty()
        before = termios.tcgetattr(slave)
        proc = subprocess.Popen(command(cli, peer.socket, "--no-history", "--prompt", "cancel me"),
                                stdin=slave, stdout=slave, stderr=slave)
        try:
            assert arrived.wait(5), "CLI did not submit its initial prompt"
            started = time.monotonic()
            proc.send_signal(signal.SIGTERM)
            assert cancelling.wait(1.5), "CLI did not notify the peer of cancellation"
            proc.send_signal(signal.SIGINT)
            assert proc.wait(timeout=3) == 128 + signal.SIGTERM, proc.returncode
            assert time.monotonic() - started < 3, "held cancellation delayed termination"
            assert termios.tcgetattr(slave) == before, "terminal was not restored"
        finally:
            release.set()
            finish(proc)
            os.close(master)
            os.close(slave)
    print("cli events: TERM survives INT during a held cancellation response: PASS", flush=True)


def check_history_lock(cli):
    with Peer(lambda request: response("unexpected answer")) as peer:
        for signum, interactive in ((signal.SIGTERM, False), (signal.SIGINT, False),
                                    (signal.SIGINT, True)):
            with tempfile.TemporaryDirectory(prefix="askai-cli-lock-") as directory:
                lock_path = directory + "/cli-init.lock"
                lock = os.open(lock_path, os.O_RDWR | os.O_CREAT | os.O_CLOEXEC, 0o600)
                try:
                    fcntl.flock(lock, fcntl.LOCK_EX)
                    master, slave = pty.openpty() if interactive else (-1, -1)
                    mode = [] if interactive else ["--single-turn"]
                    proc = subprocess.Popen(command(cli, peer.socket, "--history-repo", directory,
                                                    "--session-id", "events-lock", *mode,
                                                    "--prompt", "never submitted"),
                                            stdin=slave if interactive else subprocess.DEVNULL,
                                            stdout=slave if interactive else subprocess.PIPE,
                                            stderr=subprocess.PIPE)
                    try:
                        await_condition(proc, lambda: holds_file(proc, lock_path),
                                        "CLI did not open its contended history lock")
                        started = time.monotonic()
                        proc.send_signal(signum)
                        output, errors = proc.communicate(timeout=3)
                        expected = 1 if interactive else 128 + signum
                        assert proc.returncode == expected, (proc.returncode, output, errors)
                        assert time.monotonic() - started < 3, "history lock delayed termination"
                    finally:
                        finish(proc)
                        if interactive:
                            os.close(master)
                            os.close(slave)
                finally:
                    os.close(lock)
        assert not any(request["method"] == "tools/call" for request in peer.requests), peer.requests
    print("cli events: TERM and INT interrupt a contended history flock: PASS", flush=True)


def check_descriptor_limit(cli):
    # Limit only the exec'ed CLI. preexec_fn would be unsafe while the peer's
    # Python server threads are alive, and changing our own limit affects them.
    wrapper = ("import os, resource, sys; "
               "resource.setrlimit(resource.RLIMIT_NOFILE, (64, 64)); "
               "os.execv(sys.argv[1], sys.argv[1:])")
    with Peer(lambda request: response("fd-budget-ok")) as peer:
        entered = ("question\n" * 16 + "/exit\n").encode()
        for _ in range(3):
            result = subprocess.run([sys.executable, "-c", wrapper,
                                     *command(cli, peer.socket, "--no-history")],
                                    input=entered, capture_output=True, timeout=10)
            assert result.returncode == 0, (result.returncode, result.stderr)
            assert result.stdout == b"fd-budget-ok\n" * 16, (len(result.stdout), result.stdout[:256])
        calls = [request for request in peer.requests if request["method"] == "tools/call"]
        assert len(calls) == 48, len(calls)
    print("cli events: repeated runs and 48 turns fit RLIMIT_NOFILE=64: PASS", flush=True)


def check_descriptor_exhaustion(cli):
    wrapper = ("import os, resource, sys; "
               "limit = int(sys.argv[1]); "
               "resource.setrlimit(resource.RLIMIT_NOFILE, (limit, limit)); "
               "fds = [os.open(os.devnull, os.O_RDONLY) for _ in range(2)]; "
               "[os.set_inheritable(fd, True) for fd in fds]; "
               "os.execv(sys.argv[2], sys.argv[2:])")
    with Peer(lambda request: response("unexpected answer")) as peer:
        for limit in (8, 12):
            result = subprocess.run([sys.executable, "-c", wrapper, str(limit),
                                     *command(cli, peer.socket, "--no-history", "--single-turn",
                                              "--prompt", "not enough descriptors")],
                                    stdin=subprocess.DEVNULL, capture_output=True, timeout=3)
            assert result.returncode == 1, (limit, result.returncode, result.stderr)
            assert result.stderr, (limit, "descriptor exhaustion had no diagnostic")
            assert b"unexpected answer" not in result.stdout, (limit, result.stdout)
    print("cli events: descriptor exhaustion at limits 8/12 reports failure without hanging: PASS", flush=True)


class Terminal:
    """Keep unread PTY bytes between phase markers, including coalesced redraws."""

    def __init__(self, proc, fd):
        self.proc = proc
        self.fd = fd
        self.pending = b""

    def expect(self, marker, timeout=5):
        deadline = time.monotonic() + timeout
        while marker not in self.pending:
            remaining = deadline - time.monotonic()
            assert remaining > 0, (marker, self.pending[-4096:], self.proc.poll())
            ready, _, _ = select.select([self.fd], [], [], remaining)
            assert ready, (marker, self.pending[-4096:], self.proc.poll())
            try:
                chunk = os.read(self.fd, 65536)
            except OSError as error:
                raise AssertionError((marker, self.pending[-4096:], self.proc.poll())) from error
            assert chunk, (marker, self.pending[-4096:], self.proc.poll())
            self.pending += chunk
        end = self.pending.index(marker) + len(marker)
        self.pending = self.pending[end:]


def check_tui_drafts(cli):
    arrived = threading.Event()
    cancelled = threading.Event()
    release = threading.Event()
    prompts = []

    def reply(request):
        prompts.append(request["params"]["arguments"]["prompt"])
        if len(prompts) == 1:
            arrived.set()
            release.wait(20)
            return response("cancelled answer must not appear")
        return response("answer-two" if len(prompts) == 2 else "answer-three")

    def methods(request):
        if request.get("method") == "notifications/cancelled":
            cancelled.set()
            release.set()
            return 202, "application/json", b""
        return None

    with Peer(reply, methods) as peer:
        master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
        before = termios.tcgetattr(slave)
        before_flags = fcntl.fcntl(slave, fcntl.F_GETFL)
        environment = os.environ | {"TERM": "xterm-256color", "LC_ALL": "C.UTF-8",
                                    "INPUTRC": os.devnull}
        proc = subprocess.Popen([cli, "--socket", peer.socket, "--no-history", "--color", "off",
                                 "--prompt", "first"], stdin=slave, stdout=slave, stderr=slave,
                                env=environment)
        terminal = Terminal(proc, master)
        try:
            terminal.expect(b"\x1b[?2004h")
            assert arrived.wait(5), "CLI did not submit the first prompt"
            active = termios.tcgetattr(slave)
            proc.send_signal(signal.SIGCONT)
            time.sleep(0.05)
            assert termios.tcgetattr(slave) == active, "unsolicited CONT changed terminal settings"
            draft = "preserved draft λ"
            os.write(master, b"\x1b[200~" + draft.encode() + b"\x1b[201~")
            terminal.expect(draft.encode())
            fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 31, 100, 0, 0))
            proc.send_signal(signal.SIGWINCH)
            terminal.expect(b"\x1b[31;")

            proc.send_signal(signal.SIGTSTP)

            def stopped():
                try:
                    with open(f"/proc/{proc.pid}/status") as status:
                        return any(line.startswith("State:") and "T (stopped)" in line for line in status)
                except FileNotFoundError:
                    return False

            await_condition(proc, stopped, "CLI did not suspend")
            assert termios.tcgetattr(slave) == before, "suspension did not restore terminal settings"
            assert fcntl.fcntl(slave, fcntl.F_GETFL) == before_flags, "suspension left nonblocking I/O enabled"
            proc.send_signal(signal.SIGCONT)
            terminal.expect(b"\x1b[?1049h")
            terminal.expect(draft.encode())
            assert not termios.tcgetattr(slave)[3] & termios.ICANON, "resume left canonical input enabled"
            proc.send_signal(signal.SIGINT)
            assert cancelled.wait(5), "SIGINT did not cancel the active answer"
            terminal.expect(b"Cancelled.")
            os.write(master, b"\r")
            terminal.expect(b"answer-two")
            assert prompts == ["first", draft + "\n"], prompts
            terminal.expect(b"askai | ready")

            restored = b"undo recovered"
            os.write(master, b"\x1b[200~" + restored + b"\x1b[201~")
            terminal.expect(restored)
            proc.send_signal(signal.SIGINT)
            terminal.expect(b"> \x1b[K")
            os.write(master, b"\x1f\r")
            terminal.expect(b"answer-three")
            assert prompts[-1] == restored.decode() + "\n", prompts
            terminal.expect(b"askai | ready")
            os.write(master, b"/exit\r")
            assert proc.wait(timeout=5) == 0, proc.returncode
            assert termios.tcgetattr(slave) == before, "exit did not restore terminal settings"
        finally:
            release.set()
            if proc.poll() is None:
                proc.send_signal(signal.SIGCONT)
            finish(proc)
            os.close(master)
            os.close(slave)
    print("cli events: TUI drafts survive resize, suspend, cancellation and undoable clearing: PASS", flush=True)


def check_partial_paste(cli):
    arrived = threading.Event()
    release = threading.Event()
    marker = "delta-while-paste-is-incomplete"

    def reply(request):
        arrived.set()
        release.wait(10)
        events = [
            (None, {"jsonrpc": "2.0", "method": "notifications/askai/delta", "params": {
                "requestId": request["id"], "kind": "text", "text": marker}}),
            (None, {"jsonrpc": "2.0", "id": request["id"], "result": {
                "content": [{"type": "text", "text": marker}]}}),
        ]
        return 200, "text/event-stream", sse_wire(events)

    with Peer(reply) as peer:
        master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 100, 0, 0))
        before = termios.tcgetattr(slave)
        environment = os.environ | {"TERM": "xterm-256color", "LC_ALL": "C.UTF-8",
                                    "INPUTRC": os.devnull}
        proc = subprocess.Popen([cli, "--socket", peer.socket, "--no-history", "--color", "off",
                                 "--prompt", "reply while I paste"], stdin=slave, stdout=slave,
                                stderr=slave, env=environment)
        terminal = Terminal(proc, master)
        try:
            terminal.expect(b"\x1b[?2004h")
            assert arrived.wait(5), "CLI did not submit the first prompt"
            os.write(master, b"\x1b[200~unfinished draft")

            def input_consumed():
                available = fcntl.ioctl(slave, termios.FIONREAD, struct.pack("i", 0))
                return struct.unpack("i", available)[0] == 0

            await_condition(proc, input_consumed, "CLI did not consume the partial paste")
            # Give the callback that consumed the input time to enter
            # Readline's wait for the missing paste terminator. No more
            # input is supplied before the response must become visible.
            time.sleep(0.05)
            release.set()
            terminal.expect(marker.encode())
            assert proc.poll() is None, "partial paste unexpectedly ended the client"
            proc.send_signal(signal.SIGTERM)
            assert proc.wait(timeout=3) == 128 + signal.SIGTERM, proc.returncode
            assert termios.tcgetattr(slave) == before, "partial paste shutdown did not restore the terminal"
        finally:
            release.set()
            finish(proc)
            os.close(master)
            os.close(slave)
    print("cli events: streaming and TERM progress inside an incomplete bracketed paste: PASS", flush=True)


def check_regular_files(cli):
    answer = "regular-output-" * 20000
    with Peer(lambda request: response(answer)) as peer:
        with tempfile.TemporaryFile() as source, tempfile.TemporaryFile() as output:
            source.write(b"from regular input\n/exit\n")
            source.seek(0)
            result = subprocess.run(command(cli, peer.socket, "--no-history"), stdin=source,
                                    stdout=output, stderr=subprocess.PIPE, timeout=10)
            assert result.returncode == 0, (result.returncode, result.stderr)
            output.seek(0)
            actual = output.read()
            assert actual == (answer + "\n").encode(), (len(actual), actual[:100])
        calls = [request for request in peer.requests if request["method"] == "tools/call"]
        assert len(calls) == 1, calls
        assert calls[0]["params"]["arguments"]["prompt"] == "from regular input\n", calls[0]
    print("cli events: regular-file input and output survive worker handoffs: PASS", flush=True)


def check_slow_output(cli):
    blocks = [f"{index:04x}:" + "abcdef0123456789" * 512 for index in range(640)]
    expected = ("".join(blocks) + "\n").encode()
    assert len(expected) > 5 * 1024 * 1024

    def reply(request):
        events = [(None, {"jsonrpc": "2.0", "method": "notifications/askai/delta",
                          "params": {"requestId": request["id"], "kind": "text", "text": block}})
                  for block in blocks]
        events.append((None, {"jsonrpc": "2.0", "id": request["id"], "result": {
            "content": [{"type": "text", "text": "".join(blocks)}]}}))
        # The existing fixture fragments the bare SSE type into seven-byte
        # writes. Bulk flow control needs a fast sender, so use a valid type
        # parameter while retaining its HTTP/MCP implementation.
        return 200, "text/event-stream; charset=utf-8", sse_wire(events)

    with Peer(reply) as peer:
        proc = subprocess.Popen(command(cli, peer.socket, "--no-history", "--single-turn",
                                        "--no-reasoning", "--prompt", "large answer"),
                                stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE)
        try:
            await_condition(proc, lambda: any(request["method"] == "tools/call"
                                             for request in peer.requests), "missing large request")
            time.sleep(0.2)
            assert proc.poll() is None, "CLI exited while stdout was still blocked"
            received = bytearray()
            deadline = time.monotonic() + 15
            while True:
                remaining = deadline - time.monotonic()
                assert remaining > 0, ("slow output stalled", len(received), proc.poll())
                readable, _, _ = select.select([proc.stdout], [], [], remaining)
                assert readable, ("stdout stopped making progress", len(received), proc.poll())
                block = os.read(proc.stdout.fileno(), 65536)
                if not block:
                    break
                received.extend(block)
                time.sleep(0.002)
            errors = proc.stderr.read()
            assert proc.wait(timeout=3) == 0, (proc.returncode, errors)
            assert received == expected, (len(received), len(expected), bytes(received[:100]), errors)
        finally:
            finish(proc)
    print("cli events: throttled stdout preserves over 5 MiB of streamed output: PASS", flush=True)


def check_blocked_worker(cli):
    with Peer(lambda request: response("unexpected answer")) as peer:
        with tempfile.TemporaryDirectory(prefix="askai-cli-fifo-") as directory:
            path = directory + "/input.fifo"
            os.mkfifo(path, 0o600)
            proc = subprocess.Popen(command(cli, peer.socket, "--no-history", "--single-turn", "--file", path),
                                    stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                    stderr=subprocess.PIPE)
            writer = -1
            try:
                def connect_writer():
                    nonlocal writer
                    try:
                        writer = os.open(path, os.O_WRONLY | os.O_NONBLOCK | os.O_CLOEXEC)
                        return True
                    except OSError as error:
                        if error.errno != errno.ENXIO:
                            raise
                        return False

                await_condition(proc, connect_writer, "worker did not open FIFO input")
                await_condition(proc, lambda: holds_file(proc, path), "worker did not retain FIFO input")
                # Keep the writer open without sending any bytes. The worker
                # cannot finish its read or observe EOF until after CLI exit.
                started = time.monotonic()
                proc.send_signal(signal.SIGTERM)
                output, errors = proc.communicate(timeout=3)
                assert proc.returncode == 128 + signal.SIGTERM, (proc.returncode, output, errors)
                assert time.monotonic() - started < 3, "blocked worker prevented bounded shutdown"
                assert not any(request["method"] == "tools/call" for request in peer.requests), peer.requests
            finally:
                finish(proc)
                if writer >= 0:
                    os.close(writer)
    print("cli events: shutdown leaves a blocked FIFO worker to process exit within 3 s: PASS", flush=True)


if __name__ == "__main__":
    if len(sys.argv) < 2:
        raise SystemExit("usage: cli_events.py ASKAI_CLI [sticky lock fds exhaust tui partial files slow worker ...]")
    cli = os.path.realpath(sys.argv[1])
    checks = {"sticky": check_sticky_termination, "lock": check_history_lock,
              "fds": check_descriptor_limit, "exhaust": check_descriptor_exhaustion,
              "tui": check_tui_drafts, "partial": check_partial_paste,
              "files": check_regular_files,
              "slow": check_slow_output, "worker": check_blocked_worker}
    selected = sys.argv[2:] or checks
    for name in selected:
        if name not in checks:
            raise SystemExit(f"unknown check: {name}")
        checks[name](cli)
