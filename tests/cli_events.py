#!/usr/bin/env python3
"""CLI signal, worker ownership and bounded-output integration checks on Linux."""

import errno
import fcntl
import os
import pty
import re
import select
import shlex
import shutil
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import threading
import time

from cli import Peer, command, sse_wire, unix_available


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


def check_saved_command_lock(cli):
    with Peer(lambda request: response("unexpected answer")) as peer:
        for interrupt in (True, False):
            with tempfile.TemporaryDirectory(prefix="askai-cli-saved-lock-") as directory:
                terminal, master, slave, before = Terminal.spawn(
                    cli, peer.socket, "--history-repo", directory, "--session-id", "saved-lock")
                proc = terminal.proc
                lock = -1
                try:
                    terminal.update(b"\x1b[?2004h", b"\x1b[24;3H")
                    paths = [directory + "/" + name for name in os.listdir(directory)
                             if name.startswith("cli-") and name.endswith(".lock") and name != "cli-init.lock"]
                    assert len(paths) == 1, paths
                    lock = os.open(paths[0], os.O_RDWR | os.O_CLOEXEC)
                    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    if interrupt:
                        os.write(master, b"/help\r")
                        terminal.update(b"/regen", timeout=2)
                        os.write(master, b"/clear\r")
                        terminal.update(b"> /clear", timeout=2)
                        # Cancelling a pending save must return to command
                        # processing, even though the next save still waits.
                        try:
                            status = proc.wait(timeout=0.25)
                        except subprocess.TimeoutExpired:
                            pass
                        else:
                            raise AssertionError(("interrupting deferred history save exited", status))
                    os.write(master, b"/quit\r")
                    assert proc.wait(timeout=3) == 0, (interrupt, proc.returncode)
                    assert termios.tcgetattr(slave) == before, "history contention exit did not restore terminal settings"
                finally:
                    finish(proc)
                    if lock >= 0:
                        os.close(lock)
                    os.close(master)
                    os.close(slave)
        assert not any(request["method"] == "tools/call" for request in peer.requests), peer.requests
    print("cli events: help, interrupted history saves and quit stay responsive under writer-lock contention: PASS", flush=True)


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
        self.escape = b""
        self.cursor = self.saved = (1, 1)
        self.rows = {}

    @classmethod
    def spawn(cls, cli, socket, *arguments, columns=100, inputrc=os.devnull, color="off", environment=None):
        master, slave = pty.openpty()
        try:
            fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, columns, 0, 0))
            before = termios.tcgetattr(slave)
            environment = (os.environ if environment is None else environment) | {
                "TERM": "screen.xterm-256color", "LC_ALL": "C.UTF-8", "INPUTRC": inputrc}
            proc = subprocess.Popen([cli, "--socket", socket, "--color", color, *arguments],
                                    stdin=slave, stdout=slave, stderr=slave, env=environment)
        except BaseException:
            os.close(master)
            os.close(slave)
            raise
        return cls(proc, master), master, slave, before

    def close(self, slave):
        finish(self.proc)
        os.close(self.fd)
        os.close(slave)

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
            self.record(chunk)
            self.pending += chunk
        end = self.pending.index(marker) + len(marker)
        consumed = self.pending[:end]
        self.pending = self.pending[end:]
        return consumed

    def drain(self, timeout=0.05):
        output, self.pending = self.pending, b""
        while select.select([self.fd], [], [], timeout)[0]:
            chunk = os.read(self.fd, 65536)
            assert chunk, (output[-4096:], self.proc.poll())
            self.record(chunk)
            output += chunk
        return output

    def record(self, chunk):
        # ASCII editing checks use positioned cells; Unicode layout is checked
        # separately with tmux's terminal emulator.
        data = self.escape + chunk
        while data:
            match = re.match(rb"\x1b\[[0-?]*[ -/]*[@-~]|\x1b[^\[]|[^\x1b]+", data)
            if not match:
                break
            token, data = match[0], data[match.end():]
            position = re.fullmatch(rb"\x1b\[(\d+);(\d+)H", token)
            if position:
                self.cursor = tuple(map(int, position.groups()))
            elif token == b"\x1b7":
                self.saved = self.cursor
            elif token == b"\x1b8":
                self.cursor = self.saved
            elif token == b"\x1b[2J":
                self.rows.clear()
            elif not token.startswith(b"\x1b"):
                row, column = self.cursor
                cells = self.rows.setdefault(row, bytearray(b" " * 512))
                for byte in token:
                    if byte == 13:
                        column = 1
                    elif 32 <= byte < 127:
                        cells[column - 1] = byte
                        column += 1
                self.cursor = row, column
        self.escape = data

    def draft(self, text, point=None):
        row = struct.unpack("HHHH", fcntl.ioctl(self.fd, termios.TIOCGWINSZ, b"\0" * 8))[0]
        deadline, output = time.monotonic() + 5, self.drain()
        while bytes(self.rows.get(row, b"")).rstrip() != (b"> " + text).rstrip() or \
                (point is not None and self.cursor != (row, 3 + point)):
            assert time.monotonic() < deadline, (text, self.rows.get(row), self.cursor, output)
            output += self.drain()
        return output

    def update(self, *markers, timeout=5):
        deadline = time.monotonic() + timeout
        output = b""
        for marker in markers:
            if marker not in output:
                output += self.expect(marker, timeout=max(0, deadline - time.monotonic()))
        return output + self.drain()


class Tmux:
    """Use tmux's terminal emulator without touching any existing server."""

    def __init__(self, cli, socket, *arguments, columns=100, rows=24, color="off", inputrc=os.devnull):
        self.directory = tempfile.TemporaryDirectory(prefix="askai-cli-tmux-")
        self.socket = self.directory.name + "/tmux.sock"
        self.pane = "events:0.0"
        self.columns = columns
        self.rows = rows
        self.environment = os.environ.copy()
        self.environment.pop("TMUX", None)
        self.environment.pop("TMUX_PANE", None)
        try:
            self.run("new-session", "-d", "-s", "events", "-x", str(columns), "-y", str(rows), "sleep 30")
            self.run("set-option", "-g", "status", "off")
            self.run("set-option", "-w", "-t", "events:0", "remain-on-exit", "on")
            self.run("set-option", "-w", "-t", "events:0", "window-size", "manual")
            invocation = shlex.join(["env", "TERM=screen.xterm-256color", "LC_ALL=POSIX",
                                     "HOME=" + self.directory.name, "INPUTRC=" + inputrc,
                                     cli, "--socket", socket, "--no-history", "--color", color, *arguments])
            self.run("respawn-pane", "-k", "-t", self.pane, invocation)
        except BaseException:
            self.__exit__()
            raise

    def __enter__(self):
        return self

    def __exit__(self, *exception):
        self.run("kill-server", check=False)
        self.directory.cleanup()

    def run(self, *arguments, check=True):
        return subprocess.run(["tmux", "-u", "-S", self.socket, "-f", os.devnull, *arguments],
                              env=self.environment, capture_output=True, text=True, timeout=3, check=check)

    def capture(self):
        return self.run("capture-pane", "-p", "-t", self.pane).stdout.splitlines()

    def screen(self, condition, description, timeout=5):
        deadline = time.monotonic() + timeout
        previous = None
        while True:
            rows = self.capture()
            tiny = self.columns < 8 or self.rows < 6
            complete = len(rows) == self.rows and (tiny or all(
                row.startswith("│") and row.endswith("│") and row.count("│") == 3
                for row in rows[1:-3]))
            # capture-pane can observe the middle of a frame write. Wait
            # for a settled pane before comparing cells or splitting rows.
            if complete and rows == previous and condition(rows):
                return rows
            assert time.monotonic() < deadline, (description, rows)
            previous = rows
            time.sleep(0.02)

    def text(self, text):
        self.run("send-keys", "-t", self.pane, "-l", text)

    def keys(self, *keys):
        self.run("send-keys", "-t", self.pane, *keys)

    def resize(self, columns, rows=24):
        self.columns, self.rows = columns, rows
        self.run("resize-window", "-t", "events:0", "-x", str(columns), "-y", str(rows))

    def quit(self):
        self.keys("C-a", "C-k")
        self.text("/quit")
        self.keys("Enter")
        deadline = time.monotonic() + 3
        while True:
            status = self.run("display-message", "-p", "-t", self.pane,
                              "#{pane_dead}:#{pane_dead_status}").stdout.strip()
            if status.startswith("1:"):
                assert status == "1:0", status
                return
            assert time.monotonic() < deadline, ("CLI did not quit", status)
            time.sleep(0.02)


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
        environment = os.environ | {"TERM": "xterm-256color", "LC_ALL": "C",
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
            terminal.expect(b"ready")

            restored = b"undo recovered"
            os.write(master, b"\x1b[200~" + restored + b"\x1b[201~")
            terminal.expect(restored)
            proc.send_signal(signal.SIGINT)
            terminal.draft(b"", point=0)
            os.write(master, b"\x1f\r")
            terminal.expect(b"answer-three")
            assert prompts[-1] == restored.decode() + "\n", prompts
            terminal.expect(b"ready")
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
        terminal, master, slave, before = Terminal.spawn(
            cli, peer.socket, "--no-history", "--prompt", "reply while I paste")
        proc = terminal.proc
        try:
            terminal.expect(b"\x1b[?2004h")
            assert arrived.wait(5), "CLI did not submit the first prompt"
            terminal.update(b"working")
            os.write(master, b"z")
            typed = terminal.draft(b"z", point=1)
            positions = re.findall(rb"\x1b\[(\d+);(\d+)H", typed)
            assert positions and all(row == b"24" for row, column in positions), typed
            assert b"\x1b[?25" not in typed and b"Conversation" not in typed, typed
            os.write(master, b"x")
            typed = terminal.draft(b"zx", point=2)
            assert re.sub(rb"\x1b\[[0-?]*[ -/]*[@-~]|\x1b[78]", b"", typed) == b"x", typed
            assert b"\x1b[K" not in typed and b"\x1b[2J" not in typed, typed
            os.write(master, b"\x01\x0b")
            terminal.draft(b"", point=0)
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
            streamed = terminal.update(marker.encode())
            positions = re.findall(rb"\x1b\[(\d+);(\d+)H", streamed)
            assert not any(2 <= int(row) <= 21 and int(column) >= 60 for row, column in positions), streamed
            assert b"\x1b[?25" not in streamed and b"> " not in streamed, streamed
            assert b"\x1b7" in streamed and b"\x1b8" in streamed, streamed
            assert all(row != b"24" for row, column in positions), streamed
            assert b"\x1b[K" not in streamed and b"\x1b[2J" not in streamed, streamed
            assert "│".encode() not in streamed and b"Reasoning" not in streamed, streamed
            assert proc.poll() is None, "partial paste unexpectedly ended the client"
            proc.send_signal(signal.SIGTERM)
            assert proc.wait(timeout=3) == 128 + signal.SIGTERM, proc.returncode
            assert termios.tcgetattr(slave) == before, "partial paste shutdown did not restore the terminal"
        finally:
            release.set()
            terminal.close(slave)
    print("cli events: typing and streaming update only changed regions, and TERM interrupts an incomplete paste: PASS", flush=True)


def check_keyboard(cli):
    arrived = threading.Event()
    release = threading.Event()
    prompts = []

    def reply(request):
        prompts.append(request["params"]["arguments"]["prompt"])
        if len(prompts) == 1:
            arrived.set()
            release.wait(20)
            return response("keyboard-answer-one")
        return response("keyboard-answer-two" if len(prompts) == 2 else "keyboard-answer-three")

    with Peer(reply) as peer, tempfile.TemporaryDirectory(prefix="askai-cli-keys-") as directory:
        inputrc = directory + "/inputrc"
        with open(inputrc, "w") as bindings:
            bindings.write('"\\e[5~": history-search-backward\n"\\e[6~": history-search-forward\n')
            bindings.write('"\\C-t": accept-line\n')
        terminal, master, slave, before = Terminal.spawn(
            cli, peer.socket, "--no-history", inputrc=inputrc)
        proc = terminal.proc

        def editable(phase):
            flags = termios.tcgetattr(slave)[3]
            assert not flags & (termios.ICANON | termios.ECHO), (phase, flags)

        def type_text(text, prefix=b""):
            # Real keystrokes take separate turns. A paste or a whole line
            # written at once can conceal callback/termios handoff bugs.
            for byte in text:
                os.write(master, bytes([byte]))
                prefix += bytes([byte])
                terminal.draft(prefix)

        try:
            terminal.update(b"\x1b[?2004h", b"\x1b[24;3H")
            editable("initial prompt")
            type_text(b"/")
            for listed in range(3):
                os.write(master, b"\t")
                output = terminal.drain()
                assert b"/clear" not in output, "first Tab repeated the listing"
                if not listed:
                    time.sleep(0.65)
                    os.write(master, b"\t")
                    output = terminal.drain()
                    assert b"/clear" not in output, "widely separated Tabs were treated as a double Tab"
                os.write(master, b"\t")
                output = terminal.update(b"/clear /exit /help ")
                assert output.count(b"/clear") == 1, "second Tab redrew old completion listings"
                assert b"Completions:" not in output, "completion listing has an unnecessary heading"
            os.write(master, b"h")
            terminal.draft(b"/h")
            type_text(b"e", prefix=b"/h")
            os.write(master, b"\t")
            terminal.draft(b"/help ", point=len(b"/help "))
            os.write(master, b"x")
            terminal.draft(b"/help x")
            os.write(master, b"\x7f")
            terminal.draft(b"/help ")
            editable("completed command")
            os.write(master, b"\r")
            terminal.update(b"/regen")
            editable("after help")
            assert not prompts, prompts

            type_text(b"hi")
            os.write(master, b"\r\n")
            assert arrived.wait(5), "CRLF did not submit hi"
            terminal.expect(b"working")
            os.write(master, b"\r\n\x14")
            terminal.draft(b"")
            assert b"working" in terminal.rows[23], "blank Enter occupied the pending input slot"
            editable("while first answer is pending")
            type_text(b"queued second")
            # Exercise the inputrc accept-line binding as well as the CLI's
            # Enter command while a previous request is still running.
            os.write(master, b"\x14")
            terminal.draft(b"")
            editable("after queuing a line while busy")
            type_text(b"acx")
            os.write(master, b"\x7f")
            terminal.draft(b"ac")
            os.write(master, b"\x1b[D")
            os.write(master, b"b")
            terminal.draft(b"abc")
            os.write(master, b"\x1b[C")
            os.write(master, b"X")
            terminal.draft(b"abcX")
            os.write(master, b"\x08")
            terminal.draft(b"abc")
            os.write(master, b"\x1bOD")
            os.write(master, b"Y")
            terminal.draft(b"abYc")
            os.write(master, b"\x1bOC")
            os.write(master, b"D")
            terminal.draft(b"abYcD")
            os.write(master, b"\x7f")
            terminal.draft(b"abYc")
            os.write(master, b"\x01h\x05")
            terminal.draft(b"habYc")
            editable("after erase and cursor keys")

            release.set()
            terminal.expect(b"keyboard-answer-one")
            terminal.expect(b"keyboard-answer-two")
            terminal.expect(b"ready")
            editable("after the queued answer")
            assert prompts == ["hi\n", "queued second\n"], prompts
            os.write(master, b"\r")
            terminal.expect(b"keyboard-answer-three")
            terminal.expect(b"ready")
            editable("after third answer")
            assert prompts == ["hi\n", "queued second\n", "habYc\n"], prompts
            type_text(b"h")
            os.write(master, b"\x1b[5~")
            terminal.draft(b"habYc")
            os.write(master, b"\x1b[5~")
            terminal.draft(b"hi")
            os.write(master, b"\x1b[6~")
            terminal.draft(b"habYc")
            os.write(master, b"\x01\x0b")
            terminal.draft(b"")
            type_text(b"/qu")
            os.write(master, b"\t")
            terminal.draft(b"/quit ", point=len(b"/quit "))
            os.write(master, b"\r")
            assert proc.wait(timeout=5) == 0, proc.returncode
            assert termios.tcgetattr(slave) == before, "ordinary keyboard exit did not restore the terminal"
        finally:
            release.set()
            terminal.close(slave)
    print("cli events: ordinary Tab/Enter/erase/arrows remain editable across busy and completed turns: PASS", flush=True)


def check_queued_input(cli):
    for macro in (False, True):
        expected = ["initial", "macro first\n", "macro second\n"] if macro else \
                   ["initial", "queued before eof\n"]
        arrived = [threading.Event() for _ in expected]
        release = [threading.Event() for _ in expected]
        prompts = []

        def reply(request):
            index = len(prompts)
            prompts.append(request["params"]["arguments"]["prompt"])
            assert index < len(expected), prompts
            arrived[index].set()
            release[index].wait(15)
            return response(f"queue-answer-{index}")

        with Peer(reply) as peer, tempfile.TemporaryDirectory(prefix="askai-cli-queue-") as directory:
            inputrc = directory + "/inputrc"
            with open(inputrc, "w") as bindings:
                bindings.write('"\\C-t": "\\C-mmacro first\\C-m\\C-mmacro second\\C-m"\n')
            terminal, master, slave, before = Terminal.spawn(
                cli, peer.socket, "--no-history", "--prompt", "initial", inputrc=inputrc)
            proc = terminal.proc
            try:
                terminal.update(b"\x1b[?2004h", b"\x1b[24;3H")
                assert arrived[0].wait(5), "initial request did not arrive"
                os.write(master, b"\x14" if macro else b"queued before eof\r")
                terminal.expect(f"{len(expected) - 1} queued".encode())
                flags = termios.tcgetattr(slave)[3]
                assert not flags & (termios.ICANON | termios.ECHO), (macro, flags)
                if not macro:
                    os.write(master, b"\x04")
                    time.sleep(0.05)
                    assert proc.poll() is None, "EOF discarded the queued request"
                for index in range(len(expected)):
                    assert arrived[index].wait(5), (macro, index, prompts)
                    assert prompts == expected[:index + 1], (macro, index, prompts)
                    release[index].set()
                    terminal.expect(f"queue-answer-{index}".encode())
                if macro:
                    terminal.expect(b"ready")
                    os.write(master, b"\x04")
                assert proc.wait(timeout=5) == 0, (macro, proc.returncode)
                assert prompts == expected, (macro, prompts)
                assert termios.tcgetattr(slave) == before, "queued input exit did not restore terminal settings"
            finally:
                for event in release:
                    event.set()
                terminal.close(slave)
    print("cli events: queued input precedes EOF and multi-line macros resume in order: PASS", flush=True)


def check_chat_completion(cli):
    ask = {"name": "ask_ai", "title": "Ask AI", "inputSchema": {
        "type": "object", "properties": {"prompt": {"type": "string"},
        "messages": {"type": "array"}, "background": {"type": "boolean"}}}}
    notes = {"name": "notebook_service", "_meta": {"askai": {
        "participant": "rx6900xt", "notebooks": True}}, "inputSchema": {
            "type": "object", "properties": {"action": {"enum": ["notebooks"]},
                                              "participant": {"type": "string"}}}}

    def methods(request):
        if request["method"] == "tools/list":
            return 200, "application/json", {"result": {"tools": [ask, notes]}}
        return None

    def reply(request):
        assert request["params"]["name"] == "notebook_service", request
        assert request["params"]["arguments"] == {"action": "notebooks"}, request
        return response("rx6900xt/anonymous/upstream\nrx9070xt/anonymous/upstream\n")

    with Peer(reply, methods) as peer:
        terminal, master, slave, before = Terminal.spawn(
            cli, peer.socket, "--no-history")
        proc = terminal.proc

        try:
            terminal.update(b"\x1b[?2004h", b"\x1b[24;3H")
            os.write(master, b"/names\r")
            terminal.expect(b"Known session participants: rx6900xt rx9070xt")
            for prefix, completed in ((b"@rx", b"@rx6900xt "),
                                      (b"rx", b"rx6900xt: "),
                                      (b"/tool notebook_service participant=rx9",
                                       b"/tool notebook_service participant=rx9070xt ")):
                os.write(master, prefix)
                terminal.draft(prefix, point=len(prefix))
                os.write(master, b"\t")
                terminal.draft(completed, point=len(completed))
                os.write(master, b"x")
                terminal.draft(completed + b"x", point=len(completed) + 1)
                os.write(master, b"\x01\x0b")
                terminal.draft(b"", point=0)
            os.write(master, b"/quit\r")
            assert proc.wait(timeout=3) == 0, proc.returncode
            assert termios.tcgetattr(slave) == before, "completion exit did not restore terminal settings"
            calls = [request for request in peer.requests if request["method"] == "tools/call"]
            assert len(calls) == 1, calls
        finally:
            terminal.close(slave)
    print("cli events: chat mentions use the connected GPU while notebook completion retains all participants: PASS", flush=True)


def check_fullscreen_colors(cli):
    reasoning = "reasoning-color-visible\n" + "." * 120 + "\nwrapped-reasoning-visible"
    answer = "answer-color-visible"

    def reply(request):
        events = [(None, {"jsonrpc": "2.0", "method": "notifications/askai/delta",
                          "params": {"requestId": request["id"], "kind": "reasoning", "text": reasoning}}),
                  (None, {"jsonrpc": "2.0", "id": request["id"], "result": {
                      "content": [{"type": "text", "text": answer}]}})]
        return 200, "text/event-stream", sse_wire(events)

    def style(frame, marker, last=False):
        assert marker in frame, (marker, frame)
        foreground, background, bold = None, None, False
        position = frame.rindex(marker) if last else frame.index(marker)
        for match in re.finditer(rb"\x1b\[([0-9;]*)m", frame[:position]):
            codes = iter(int(value or b"0") for value in match[1].split(b";"))
            for code in codes:
                if code == 0:
                    foreground, background, bold = None, None, False
                elif code == 1:
                    bold = True
                elif code == 22:
                    bold = False
                elif 30 <= code <= 37 or 90 <= code <= 97:
                    foreground = code
                elif code == 39:
                    foreground = None
                elif code == 49:
                    background = None
                elif code in (38, 48):
                    assert next(codes) == 5, ("unexpected color encoding", match[0])
                    index = next(codes)
                    if code == 38:
                        foreground = index
                    else:
                        background = index
        return foreground, background, bold

    for color, no_color in (("on", False), ("off", False), ("auto", True)):
        with Peer(reply) as peer:
            environment = os.environ.copy()
            environment.pop("NO_COLOR", None)
            if no_color:
                environment["NO_COLOR"] = "1"
            terminal, master, slave, before = Terminal.spawn(
                cli, peer.socket, "--no-history", "--prompt", "color request", color=color, environment=environment)
            proc = terminal.proc

            def check(output, draft=False):
                if color == "on":
                    assert style(output, b"Connected to ask_ai.")[:2] == (110, None), output
                    assert style(output, b"reasoning-color-visible")[:2] == (146, None), output
                    assert style(output, b"wrapped-reasoning-visible")[:2] == (146, None), output
                    assert style(output, answer.encode())[:2] == (252, None), output
                    if draft:
                        assert style(output, b"Error:") == (203, None, False), output
                        assert style(output, b"draft-color-visible")[:2] == (231, None), output
                        assert style(output, b"> ", last=True)[:2] == (229, None), output
                else:
                    codes = [int(value or b"0") for match in re.finditer(rb"\x1b\[([0-9;]*)m", output)
                             for value in match[1].split(b";")]
                    assert not any(30 <= code <= 37 or 90 <= code <= 97 or code in (38, 48)
                                   for code in codes), (color, no_color, output)

            try:
                output = terminal.update(answer.encode(), b"ready")
                check(output)
                os.write(master, b"/tools absent\r")
                output += terminal.update(b"No accessible tool named absent")
                os.write(master, b"draft-color-visible")
                output += terminal.update(b"draft-color-visible")
                check(output, draft=True)
                fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 80, 0, 0))
                proc.send_signal(signal.SIGWINCH)
                check(terminal.update(b"draft-color-visible"), draft=True)
                os.write(master, b"\x01\x0b/quit\r")
                assert proc.wait(timeout=3) == 0, (color, no_color, proc.returncode)
                assert termios.tcgetattr(slave) == before, "color exit did not restore terminal settings"
            finally:
                terminal.close(slave)
    print("cli events: full-screen colors survive redraw and obey --color off and NO_COLOR: PASS", flush=True)


def check_terminal_cells(cli):
    if not shutil.which("tmux"):
        print("cli events: terminal-cell wrapping: SKIP (tmux unavailable)", flush=True)
        return
    # At 80 columns the conversation pane has 46 interior cells. Each
    # boundary case ends at or immediately after its right border.
    lines = ["A" * 45 + "Z", "B" * 45 + "YX", "界" * 23, "\t" + "C" * 37 + "Q"]
    answer = "\n".join(lines + ["cells-done"])
    with Peer(lambda request: response(answer)) as peer, \
            Tmux(cli, peer.socket, "--prompt", "wrap cells", columns=80) as terminal:
        for columns, interior in ((80, 46), (40, 22)):
            if columns == 40:
                terminal.resize(columns)
                terminal.text("resize-marker")
            marker = "resize-marker" if columns == 40 else "cells-done"
            output = terminal.screen(lambda rows: marker in "\n".join(rows) and "askai | ready" in rows[-2],
                                     "wrapped output did not redraw")
            expected = []
            for text in lines:
                if text.startswith("界"):
                    expected.extend(text[i:i + interior // 2] for i in range(0, len(text), interior // 2))
                else:
                    text = text.expandtabs(8)
                    expected.extend(text[i:i + interior] for i in range(0, len(text), interior))
            expected.append("cells-done")
            actual = [row.split("│")[1].rstrip() for row in output[1:-3]]
            assert "\n".join(expected) in "\n".join(actual), (columns, "rightmost pane cells were lost", output)
            assert all(row.endswith("│") for row in output[1:-3]), ("right border was erased", output)
            for text in ("x" * (columns - 4), "界" * ((columns - 4) // 2)):
                terminal.keys("C-a", "C-k")
                terminal.text(text)
                terminal.screen(lambda rows: rows[-1] == "> " + text,
                                "draft scrolled before reaching the right edge")
                cursor = terminal.run("display-message", "-p", "-t", terminal.pane,
                                      "#{cursor_x},#{cursor_y}").stdout.strip()
                assert cursor == f"{columns - 2},23", (text, cursor)
            terminal.keys("C-a", "C-k")
            terminal.text("x" * columns + "suffix")
            terminal.screen(lambda rows: rows[-1].endswith("suffix"), "long draft did not scroll into view")
            terminal.keys("Left", "Left")
            terminal.screen(lambda rows: terminal.run("display-message", "-p", "-t", terminal.pane,
                                                      "#{cursor_x}").stdout.strip() == str(columns - 3),
                            "cursor did not move left within the scrolled draft")
            before = terminal.run("display-message", "-p", "-t", terminal.pane, "#{cursor_x}").stdout.strip()
            terminal.text("q")
            terminal.screen(lambda rows: "q" in rows[-1], "editing a scrolled draft did not update input")
            after = terminal.run("display-message", "-p", "-t", terminal.pane, "#{cursor_x}").stdout.strip()
            assert int(after) == int(before) + 1, ("editing jumped the viewport", before, after)
            terminal.keys("C-a", "C-k")
        terminal.quit()
    print("cli events: real terminal cells preserve full-width ASCII, wide text and tabs through resize: PASS", flush=True)


def check_panels(cli):
    if not shutil.which("tmux"):
        print("cli events: panel cells and navigation: SKIP (tmux unavailable)", flush=True)
        return
    initializing = threading.Event()
    release = threading.Event()
    prompts = []
    ask = {"name": "ask_ai", "title": "Ask AI", "inputSchema": {"type": "object", "properties": {
        "prompt": {"type": "string"}, "messages": {"type": "array"}, "background": {"type": "boolean"}}}}
    tools = [ask, {"name": "git", "inputSchema": {"type": "object", "properties": {
        "command": {"type": "string", "enum": ["log", "status"]}}}},
        {"name": "help", "inputSchema": {"type": "object", "properties": {}}},
        {"name": "lookup", "inputSchema": {"type": "object", "properties": {}}}]

    def methods(request):
        if request["method"] == "initialize":
            initializing.set()
            release.wait(10)
        elif request["method"] == "tools/list":
            return 200, "application/json", {"result": {"tools": tools}}
        return None

    def reply(request):
        prompts.append(request["params"]["arguments"]["prompt"])
        reasoning = "\n".join(f"reason-{index:02}" for index in range(50)) if len(prompts) == 1 else "reason-new"
        answer = "\n".join(f"answer-{index:02}" for index in range(50)) if len(prompts) == 1 else "answer-new"
        events = [(None, {"jsonrpc": "2.0", "method": "notifications/askai/delta", "params": {
            "requestId": request["id"], "kind": "reasoning", "text": reasoning}}),
            (None, {"jsonrpc": "2.0", "id": request["id"], "result": {
                "content": [{"type": "text", "text": answer}]}})]
        return 200, "text/event-stream", sse_wire(events)

    def pane(rows, index):
        return [row.split("│")[index + 1].rstrip() for row in rows[1:-3]]

    with Peer(reply, methods) as peer:
        try:
            with Tmux(cli, peer.socket, "--prompt", "initial panels", color="on", inputrc="") as terminal:
                assert initializing.wait(5), "initialization did not reach the peer"
                early = terminal.screen(lambda rows: len(rows) == 24 and rows[-1] == ">",
                                        "input row was not drawn while initialization waited")
                cursor = terminal.run("display-message", "-p", "-t", terminal.pane,
                                      "#{cursor_x},#{cursor_y}").stdout.strip()
                assert cursor == "2,23", ("startup cursor was not after the bottom prompt", cursor, early)
                release.set()
                output = terminal.screen(lambda rows: "answer-49" in "\n".join(pane(rows, 0)) and
                                         "reason-49" in "\n".join(pane(rows, 1)) and "ready" in rows[-2],
                                         "answer and reasoning did not reach separate panes")
                assert "Conversation" in output[0] and "Reasoning" in output[0], output
                assert "Alt-Left/Right" in output[-2] and "Alt-Up/Down" in output[-2], output
                assert not any("reason-" in row for row in pane(output, 0)), output
                assert not any("answer-" in row for row in pane(output, 1)), output
                assert all(len(row) == 100 and row[0] == row[59] == row[-1] == "│"
                           for row in output[1:-3]), ("pane borders are misplaced", output)

                left = pane(output, 0)
                right = pane(output, 1)
                terminal.keys("F6")
                for up, down in ((["C-PPage"], ["C-NPage"]), (["M-Up"], ["M-Down"]),
                                 (["-H", "1b", "1b", "5b", "41"], ["-H", "1b", "1b", "5b", "42"]),
                                 (["-H", "1b", "1b", "4f", "41"], ["-H", "1b", "1b", "4f", "42"])):
                    terminal.keys(*up)
                    scrolled = terminal.screen(lambda rows: pane(rows, 1) != right,
                                               "reasoning pane did not scroll: " + " ".join(up))
                    assert pane(scrolled, 0) == left, "scrolling reasoning moved the conversation"
                    terminal.keys(*down)
                    restored = terminal.screen(lambda rows: pane(rows, 1) == right,
                                               "reasoning viewport was not restored: " + " ".join(down))
                    assert pane(restored, 0) == left, "scrolling reasoning down moved the conversation"
                terminal.keys("C-PPage")
                scrolled = terminal.screen(lambda rows: pane(rows, 1) != right,
                                           "reasoning pane did not scroll back up")
                right = pane(scrolled, 1)
                terminal.text("next panel turn")
                terminal.keys("Enter")
                updated = terminal.screen(lambda rows: "answer-new" in "\n".join(pane(rows, 0)) and
                                          "ready" in rows[-2], "conversation did not follow its new answer")
                assert pane(updated, 1) == right, "new reasoning moved a scrolled viewport"
                for end in (["C-End"], ["-H", "1b", "5b", "34", "3b", "35", "7e"],
                            ["-H", "1b", "5b", "38", "3b", "35", "7e"], ["-H", "1b", "5b", "38", "5e"]):
                    terminal.keys("C-PPage")
                    terminal.screen(lambda rows: "reason-new" not in "\n".join(pane(rows, 1)),
                                    "reasoning did not scroll before Ctrl-End")
                    terminal.keys(*end)
                    followed = terminal.screen(lambda rows: "reason-new" in "\n".join(pane(rows, 1)),
                                               "Ctrl-End did not resume following reasoning: " + " ".join(end))
                right = pane(followed, 1)
                left = pane(followed, 0)
                terminal.text("retained draft")
                terminal.keys("M-Left", "M-Left", "C-PPage")
                scrolled = terminal.screen(lambda rows: pane(rows, 0) != left and
                                           "* Conversation" in rows[0] and
                                           rows[-1].startswith("> retained draft"),
                                           "conversation pane did not scroll independently")
                assert pane(scrolled, 1) == right, "scrolling the conversation moved reasoning"
                for columns in (60, 40):
                    terminal.resize(columns)
                    narrow = terminal.screen(lambda rows: len(rows[1]) == columns and
                                             "Conversation" in rows[0] and "Reasoning" in rows[0],
                                             "narrow terminal did not keep both panes visible")
                    divider = (columns - 3) * 3 // 5 + 1
                    assert all(len(row) == columns and row[divider] == "│" for row in narrow[1:-3]), narrow
                    for key, title in (("M-Right", "Reasoning"), ("M-Left", "Conversation")):
                        terminal.keys(key, key)
                        selected = terminal.screen(lambda rows: "* " + title in rows[0],
                                                   key + " did not select its pane")
                        assert selected[1:-3] == narrow[1:-3], "pane selection moved the viewports"
                        assert selected[-1].startswith("> retained draft"), selected
                for prefix in ("5b", "4f"):
                    for arrow, title in (("43", "Reasoning"), ("44", "Conversation")):
                        terminal.keys("-H", "1b", "1b", prefix, arrow)
                        selected = terminal.screen(lambda rows: "* " + title in rows[0],
                                                   "escape-prefixed arrow did not select " + title)
                        assert selected[1:-3] == narrow[1:-3] and selected[-1].startswith("> retained draft"), selected
                terminal.keys("M-Right")

                terminal.keys("C-a", "C-k")
                terminal.text("/tool ")
                terminal.keys("Tab", "Tab")
                listing = terminal.screen(lambda rows: "Conversation" in rows[0] and
                                          "git help lookup" in "\n".join(pane(rows, 0)),
                                          "completion choices were not visible beside narrow reasoning")
                assert "/tool git" not in "\n".join(pane(listing, 0)), listing
                terminal.resize(100)
                terminal.keys("C-a", "C-k")
                terminal.text("/tool git command=")
                terminal.keys("Tab", "Tab")
                listing = terminal.screen(lambda rows: "log status" in "\n".join(pane(rows, 0)),
                                          "value completion did not display short labels")
                assert "command=log" not in "\n".join(pane(listing, 0)), listing
                terminal.keys("C-a", "C-k")
                terminal.text("tiny draft preserved")
                terminal.resize(7, 5)
                terminal.screen(lambda rows: rows[-1].startswith("< ") and not any(row.strip() for row in rows[:-1]),
                                "tiny terminal did not switch to its bottom input row")
                position = terminal.run("display-message", "-p", "-t", terminal.pane,
                                        "#{cursor_x},#{cursor_y},#{pane_width},#{pane_height}").stdout.strip()
                x, y, width, height = map(int, position.split(","))
                assert (width, height) == (7, 5) and 0 <= x < width and y == height - 1, position
                terminal.resize(100)
                restored = terminal.screen(lambda rows: "Conversation" in rows[0] and "Reasoning" in rows[0] and
                                           rows[-1].startswith("> tiny draft preserved"),
                                           "normal panes or the draft were lost after a tiny resize")
                assert "reason-new" in "\n".join(pane(restored, 1)), restored
                terminal.keys("C-l")
                terminal.screen(lambda rows: rows == restored, "Ctrl-L failed to restore the cached display")
                assert prompts == ["initial panels", "next panel turn\n"], prompts
                terminal.quit()
        finally:
            release.set()
    print("cli events: early bottom input, separate panes, independent scrolling, Alt-arrow selection and completion labels: PASS", flush=True)


def check_tool_summaries(cli):
    cases = [(158, "é"), (159, "é"), (157, "界"), (158, "界")]
    tools = [{"name": f"tool_{i}", "description": "a" * count + character + "overflow",
              "inputSchema": {"type": "object"}} for i, (count, character) in enumerate(cases)]
    tools[0].update(title="Ask AI", inputSchema={"type": "object", "properties": {
        "prompt": {"type": "string"}, "messages": {"type": "array"}, "background": {"type": "boolean"}}})

    def methods(request):
        if request["method"] == "tools/list":
            return 200, "application/json", {"result": {"tools": tools}}
        return None

    with Peer(lambda request: response("unexpected"), methods) as peer:
        result = subprocess.run(command(cli, peer.socket, "--no-history"), input=b"/tools\n/quit\n",
                                capture_output=True, timeout=5)
        assert result.returncode == 0, (result.returncode, result.stdout, result.stderr)
        for tool in tools:
            summary = tool["description"].encode()[:160].decode("utf-8", errors="ignore")
            assert (tool["name"] + "  " + summary + "\n").encode() in result.stdout, result.stdout
    print("cli events: tool summaries preserve UTF-8 at the 160-byte boundary: PASS", flush=True)


def check_busy_commands(cli):
    arrived = threading.Event()
    release = threading.Event()
    prompts = []
    ask = {"name": "ask_ai", "title": "Ask AI", "description": "cached-tool-description",
           "inputSchema": {"type": "object", "properties": {
               "prompt": {"type": "string"}, "messages": {"type": "array"},
               "background": {"type": "boolean"}}}}
    notes = {"name": "notebook_service", "_meta": {"askai": {
        "participant": "cached-person", "notebooks": True}}, "inputSchema": {
            "type": "object", "properties": {"action": {"enum": ["notebooks"]}}}}

    def methods(request):
        if request["method"] == "tools/list":
            return 200, "application/json", {"result": {"tools": [ask, notes]}}
        return None

    def reply(request):
        if request["params"]["name"] == "notebook_service":
            return response("")
        prompts.append(request["params"]["arguments"]["prompt"])
        if len(prompts) == 1:
            return response("history-answer-visible")
        if len(prompts) == 2:
            arrived.set()
            release.wait(20)
            return response("held-answer-finished")
        return response("queued-answer-finished")

    with Peer(reply, methods) as peer:
        terminal, master, slave, before = Terminal.spawn(
            cli, peer.socket, "--no-history", "--prompt", "history-seed", columns=160)
        proc = terminal.proc
        try:
            terminal.expect(b"history-answer-visible")
            terminal.expect(b"ready")
            os.write(master, b"held-generation\r")
            assert arrived.wait(5), "generation did not start"
            terminal.expect(b"working")
            requests = len(peer.requests)
            for queued in (False, True):
                if queued:
                    os.write(master, b"queued-prompt-preserved\r")
                    terminal.expect(b"1 queued")
                for name, marker in ((b"/help", b"/regen"),
                                     (b"/history", b"history-answer-visible"),
                                     (b"/names", b"cached-person"),
                                     (b"/tools", b"cached-tool-description")):
                    # A distinct second-round echo avoids matching the same
                    # command's older output while the screen is redrawn.
                    entered = b" " * queued + name
                    os.write(master, entered + b"\r")
                    terminal.update(b"> " + entered, marker, timeout=2)
                    assert proc.poll() is None, (name, proc.returncode)
                    flags = termios.tcgetattr(slave)[3]
                    assert not flags & (termios.ICANON | termios.ECHO), (name, flags)
                    assert len(peer.requests) == requests, (name, peer.requests[requests:])
                    assert prompts == ["history-seed", "held-generation\n"], prompts
            release.set()
            terminal.expect(b"held-answer-finished")
            terminal.expect(b"queued-answer-finished")
            terminal.expect(b"ready")
            assert prompts == ["history-seed", "held-generation\n", "queued-prompt-preserved\n"], prompts
            os.write(master, b"/quit\r")
            assert proc.wait(timeout=3) == 0, proc.returncode
            assert termios.tcgetattr(slave) == before, "local-command exit did not restore terminal settings"
        finally:
            release.set()
            terminal.close(slave)
    print("cli events: local commands run during generation and preserve a queued prompt: PASS", flush=True)


def check_concurrent_tools(cli):
    ask = {"name": "ask_ai", "title": "Ask AI", "inputSchema": {
        "type": "object", "properties": {"prompt": {"type": "string"},
        "messages": {"type": "array"}, "background": {"type": "boolean"}}}}
    probe = {"name": "probe", "inputSchema": {"type": "object", "properties": {
        "token": {"type": "string"}}}}
    for mode in ("complete", "interrupt", "idle-interrupt", "quit", "saturated"):
        names = ("generation", "alpha", "beta", "gamma")
        if mode == "saturated":
            names += tuple(f"held-{index}" for index in range(12))
        arrived = {name: threading.Event() for name in names}
        release = {name: threading.Event() for name in names}
        calls, cancelled = {}, set()

        def methods(request):
            if request["method"] == "tools/list":
                return 200, "application/json", {"result": {"tools": [ask, probe]}}
            if request["method"] == "notifications/cancelled":
                cancelled.add(request["params"]["requestId"])
                return 202, "application/json", b""
            return None

        def reply(request):
            arguments = request["params"]["arguments"]
            if request["params"]["name"] == "probe":
                name = arguments["token"]
            elif arguments["prompt"] == "seed":
                name = "generation"
            else:
                calls["history"] = request
                return response("history-checked")
            calls[name] = request
            arrived[name].set()
            release[name].wait(20)
            if name != "generation":
                return response("result-" + name)
            events = [(None, {"jsonrpc": "2.0", "method": "notifications/askai/delta",
                "params": {"requestId": request["id"], "kind": kind, "text": text}})
                for kind, text in (("reasoning", "generation-reasoning"),
                                   ("text", "generation-answer"))]
            events.append((None, {"jsonrpc": "2.0", "id": request["id"], "result": {
                "content": [{"type": "text", "text": "generation-answer"}]}}))
            return 200, "text/event-stream", sse_wire(events)

        with Peer(reply, methods) as peer:
            terminal, master, slave, before = Terminal.spawn(
                cli, peer.socket, "--no-history", "--prompt", "seed", columns=140)
            proc = terminal.proc
            try:
                terminal.update(b"\x1b[?2004h", b"\x1b[24;3H")
                assert arrived["generation"].wait(5), "generation did not start"
                os.write(master, b"/tool probe token=alpha\r")
                assert arrived["alpha"].wait(2), "manual tool waited for generation"
                # One input burst exercises repeated submission before a
                # previous tool or the generation has produced a result.
                os.write(master, b"/tool probe token=beta\r/tool probe token=gamma\r")
                for name in ("beta", "gamma"):
                    assert arrived[name].wait(2), (name, "tool waited for an earlier tool")
                if mode == "saturated":
                    for name in names[4:]:
                        os.write(master, f"/tool probe token={name}\r".encode())
                        assert arrived[name].wait(2), (name, "held calls exhausted transport capacity")
                    os.write(master, b"/tool probe token=overflow\r")
                    terminal.expect(b"Too many active requests", timeout=2)
                    assert len(calls) == len(names), calls
                    proc.send_signal(signal.SIGINT)
                    terminal.expect(b"Cancelled.", timeout=2)
                    await_condition(proc, lambda: bool(cancelled), "saturation blocked cancellation", timeout=2)
                    assert cancelled == {calls["generation"]["id"]}, cancelled
                    assert not any(event.is_set() for event in release.values())
                assert len({calls[name]["id"] for name in names}) == len(names), calls
                flags = termios.tcgetattr(slave)[3]
                assert not flags & (termios.ICANON | termios.ECHO), flags
                if mode not in ("quit", "saturated"):
                    if mode == "interrupt":
                        proc.send_signal(signal.SIGINT)
                        terminal.expect(b"Cancelled.", timeout=2)
                        await_condition(proc, lambda: bool(cancelled), "no foreground cancellation")
                        assert cancelled == {calls["generation"]["id"]}, cancelled
                    # Complete out of submission order, including the chat
                    # response while two independent tools remain held.
                    release["beta"].set()
                    terminal.expect(b"result-beta", timeout=2)
                    if mode != "interrupt":
                        release["generation"].set()
                        terminal.update(b"generation-reasoning", b"generation-answer", timeout=2)
                    if mode == "idle-interrupt":
                        proc.send_signal(signal.SIGINT)
                        await_condition(proc, lambda: bool(cancelled), "no manual-tool cancellation")
                        assert cancelled == {calls["gamma"]["id"]}, cancelled
                    for name in (("alpha",) if mode == "idle-interrupt" else ("gamma", "alpha")):
                        release[name].set()
                        terminal.expect(("result-" + name).encode(), timeout=2)
                    os.write(master, b"history-probe\r")
                    terminal.expect(b"history-checked", timeout=2)
                    history = [
                        {"role": "user", "content": "seed"},
                        {"role": "assistant", "content": "generation-answer"}] if mode != "interrupt" else []
                    assert calls["history"]["params"]["arguments"]["messages"] == history, calls["history"]
                    interrupted = {"interrupt": "generation", "idle-interrupt": "gamma"}.get(mode)
                    expected = {calls[interrupted]["id"]} if interrupted else set()
                    assert cancelled == expected, cancelled
                started = time.monotonic()
                os.write(master, b"/quit\r")
                assert proc.wait(timeout=3) == 0, (mode, proc.returncode, terminal.drain()[-12000:])
                assert time.monotonic() - started < 3, "quit waited for held requests"
                assert termios.tcgetattr(slave) == before, "concurrent tools left terminal settings changed"
            finally:
                for event in release.values():
                    event.set()
                terminal.close(slave)
    print("cli events: concurrent tools, burst input, independent streaming, cancellation and quit: PASS", flush=True)


def check_queue_overflow(cli):
    arrived, release = threading.Event(), threading.Event()

    def reply(request):
        arrived.set()
        release.wait(20)
        return response("held-generation")

    with Peer(reply) as peer, tempfile.TemporaryDirectory(prefix="askai-cli-overflow-") as directory:
        inputrc = directory + "/inputrc"
        with open(inputrc, "w") as output:
            output.write('"\\C-t": accept-line\n')
        terminal, master, slave, before = Terminal.spawn(
            cli, peer.socket, "--no-history", "--prompt", "held", inputrc=inputrc)
        proc = terminal.proc
        try:
            terminal.update(b"\x1b[?2004h", b"\x1b[24;3H")
            assert arrived.wait(5), "generation did not start"
            os.write(master, b"".join(f"queued-{index}\r".encode() for index in range(32)))
            terminal.expect(b"32 queued")
            terminal.draft(b"")
            os.write(master, b"overflow-draft")
            terminal.draft(b"overflow-draft")
            for enter in (b"\r", b"\x14"):
                os.write(master, enter)
                terminal.expect(b"Input queue full; draft kept.")
                terminal.draft(b"overflow-draft", point=len(b"overflow-draft"))
                flags = termios.tcgetattr(slave)[3]
                assert not flags & (termios.ICANON | termios.ECHO), flags
            os.write(master, b"\x01\x0b/quit\r")
            assert proc.wait(timeout=3) == 0, proc.returncode
            assert termios.tcgetattr(slave) == before, "full queue exit did not restore terminal settings"
            calls = [request for request in peer.requests if request["method"] == "tools/call"]
            assert len(calls) == 1, calls
        finally:
            release.set()
            terminal.close(slave)
    print("cli events: bounded input queue reports overflow, retains editable drafts and permits quit: PASS", flush=True)


def check_busy_quit(cli):
    for name in (b"/quit", b"/exit"):
        arrived = threading.Event()
        release = threading.Event()

        def reply(request):
            arrived.set()
            release.wait(15)
            return response("answer must not be needed to quit")

        def methods(request):
            if request["method"] == "notifications/cancelled":
                release.wait(15)
            return None

        with Peer(reply, methods) as peer:
            terminal, master, slave, before = Terminal.spawn(
                cli, peer.socket, "--no-history", "--prompt", "held-generation")
            proc = terminal.proc
            try:
                terminal.update(b"\x1b[?2004h", b"\x1b[24;3H")
                assert arrived.wait(5), "generation did not start"
                os.write(master, b"queued-prompt\r")
                terminal.expect(b"1 queued")
                started = time.monotonic()
                os.write(master, name + b"\r")
                assert proc.wait(timeout=3) == 0, (name, proc.returncode)
                assert time.monotonic() - started < 3, (name, "quit waited for generation")
                assert termios.tcgetattr(slave) == before, (name, "terminal settings were not restored")
                calls = [request for request in peer.requests if request["method"] == "tools/call"]
                assert len(calls) == 1, (name, calls)
            finally:
                release.set()
                terminal.close(slave)
    print("cli events: /quit and /exit stop held generation with a queued prompt and restore the terminal: PASS", flush=True)


def check_interrupt_commands(cli):
    for name in (b"/clear", b"/regen"):
        arrived = threading.Event()
        cancelled = threading.Event()
        release = threading.Event()
        calls = []

        def reply(request):
            calls.append(request)
            index = len(calls) - 1
            if index == 2:
                arrived.set()
                release.wait(20)
                return response("interrupted answer must not enter history")
            return response(("first-seed-answer", "second-seed-answer", "unused",
                             "replacement-answer", "probe-answer")[index])

        def methods(request):
            if request["method"] == "notifications/cancelled":
                assert request["params"]["requestId"] == calls[2]["id"], request
                cancelled.set()
            return None

        with Peer(reply, methods) as peer:
            terminal, master, slave, before = Terminal.spawn(
                cli, peer.socket, "--no-history", "--prompt", "first-seed")
            proc = terminal.proc
            try:
                for marker, entered in ((b"first-seed-answer", b"second-seed\r"),
                                        (b"second-seed-answer", b"held-turn\r")):
                    terminal.expect(marker)
                    terminal.expect(b"ready")
                    os.write(master, entered)
                assert arrived.wait(5), "generation did not start"
                terminal.expect(b"working")
                if name == b"/clear":
                    os.write(master, b"after-clear\r")
                    terminal.expect(b"1 queued")
                os.write(master, name + b"\r")
                assert cancelled.wait(2), (name, "active generation was not cancelled")
                if name == b"/clear":
                    terminal.expect(b"Conversation cleared.", timeout=2)
                terminal.expect(b"replacement-answer", timeout=2)
                terminal.expect(b"ready")
                assert not release.is_set(), "held response was released before interruption completed"
                assert len(calls) == 4, (name, calls)
                arguments = calls[3]["params"]["arguments"]
                first = [{"role": "user", "content": "first-seed"},
                         {"role": "assistant", "content": "first-seed-answer"}]
                if name == b"/clear":
                    assert arguments["prompt"] == "after-clear\n", arguments
                    assert arguments["messages"] == [], arguments
                else:
                    assert arguments["prompt"] == "second-seed\n", arguments
                    assert arguments["messages"] == first, arguments
                    os.write(master, b"history-probe\r")
                    terminal.expect(b"probe-answer")
                    terminal.expect(b"ready")
                    assert len(calls) == 5, calls
                    probe = calls[4]["params"]["arguments"]
                    assert probe["prompt"] == "history-probe\n", probe
                    assert probe["messages"] == first + [
                        {"role": "user", "content": "second-seed\n"},
                        {"role": "assistant", "content": "replacement-answer"}], probe
                os.write(master, b"/quit\r")
                assert proc.wait(timeout=3) == 0, (name, proc.returncode)
                assert termios.tcgetattr(slave) == before, (name, "terminal settings were not restored")
            finally:
                release.set()
                terminal.close(slave)
    print("cli events: /clear and /regen cancel active work and preserve the intended conversation history: PASS", flush=True)


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
        raise SystemExit("usage: cli_events.py ASKAI_CLI [sticky lock saved-lock fds exhaust tui partial keys queue overflow mentions colors cells panels summaries commands concurrent quit interrupt files slow worker ...]")
    if not unix_available():
        raise SystemExit(0)
    cli = os.path.realpath(sys.argv[1])
    checks = {"sticky": check_sticky_termination, "lock": check_history_lock,
              "saved-lock": check_saved_command_lock,
              "fds": check_descriptor_limit, "exhaust": check_descriptor_exhaustion,
              "tui": check_tui_drafts, "partial": check_partial_paste, "keys": check_keyboard,
              "queue": check_queued_input, "overflow": check_queue_overflow,
              "mentions": check_chat_completion, "colors": check_fullscreen_colors,
              "cells": check_terminal_cells, "panels": check_panels, "summaries": check_tool_summaries,
              "commands": check_busy_commands, "concurrent": check_concurrent_tools, "quit": check_busy_quit,
              "interrupt": check_interrupt_commands, "files": check_regular_files,
              "slow": check_slow_output, "worker": check_blocked_worker}
    selected = sys.argv[2:] or checks
    for name in selected:
        if name not in checks:
            raise SystemExit(f"unknown check: {name}")
        checks[name](cli)
