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


def check_saved_command_lock(cli):
    with Peer(lambda request: response("unexpected answer")) as peer:
        for interrupt in (True, False):
            with tempfile.TemporaryDirectory(prefix="askai-cli-saved-lock-") as directory:
                master, slave = pty.openpty()
                fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 100, 0, 0))
                before = termios.tcgetattr(slave)
                environment = os.environ | {"TERM": "screen.xterm-256color", "LC_ALL": "C.UTF-8",
                                            "INPUTRC": os.devnull}
                proc = subprocess.Popen([cli, "--socket", peer.socket, "--color", "off",
                                         "--history-repo", directory, "--session-id", "saved-lock"],
                                        stdin=slave, stdout=slave, stderr=slave, env=environment)
                terminal = Terminal(proc, master)
                lock = -1
                try:
                    terminal.expect(b"\x1b[?2004h")
                    terminal.expect(b"> \x1b[K\x1b[24;")
                    paths = [directory + "/" + name for name in os.listdir(directory)
                             if name.startswith("cli-") and name.endswith(".lock") and name != "cli-init.lock"]
                    assert len(paths) == 1, paths
                    lock = os.open(paths[0], os.O_RDWR | os.O_CLOEXEC)
                    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    if interrupt:
                        os.write(master, b"/help\r")
                        terminal.expect(b"> /help\x1b[K\r", timeout=2)
                        terminal.expect(b"/regen", timeout=2)
                        terminal.expect(b"> \x1b[K\x1b[24;", timeout=2)
                        os.write(master, b"/clear\r")
                        terminal.expect(b"> /clear\x1b[K\r", timeout=2)
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
        consumed = self.pending[:end]
        self.pending = self.pending[end:]
        return consumed


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
        master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 100, 0, 0))
        before = termios.tcgetattr(slave)
        inputrc = directory + "/inputrc"
        with open(inputrc, "w") as bindings:
            bindings.write('"\\e[5~": history-search-backward\n"\\e[6~": history-search-forward\n')
            bindings.write('"\\C-t": accept-line\n')
        environment = os.environ | {"TERM": "screen.xterm-256color", "LC_ALL": "C.UTF-8",
                                    "INPUTRC": inputrc}
        proc = subprocess.Popen([cli, "--socket", peer.socket, "--no-history", "--color", "off"],
                                stdin=slave, stdout=slave, stderr=slave, env=environment)
        terminal = Terminal(proc, master)

        def editable(phase):
            flags = termios.tcgetattr(slave)[3]
            assert not flags & (termios.ICANON | termios.ECHO), (phase, flags)

        def draft(text, point=None):
            # Require the actual input row, not an occurrence in a previous
            # transcript or an echoed command. All drafts fit one row.
            cursor = b"" if point is None else str(3 + point).encode() + b"H"
            return terminal.expect(b"> " + text + b"\x1b[K\x1b[24;" + cursor)

        def type_text(text, prefix=b""):
            # Real keystrokes take separate turns. A paste or a whole line
            # written at once can conceal callback/termios handoff bugs.
            for byte in text:
                os.write(master, bytes([byte]))
                prefix += bytes([byte])
                draft(prefix)

        try:
            terminal.expect(b"\x1b[?2004h")
            draft(b"")
            editable("initial prompt")
            type_text(b"/")
            for listed in range(3):
                os.write(master, b"\t")
                frame = draft(b"/").rsplit(b"\x1b[?25l\x1b[H", 1)[-1]
                assert frame.count(b"/clear") == listed, "first Tab repeated the listing"
                if not listed:
                    time.sleep(0.65)
                    os.write(master, b"\t")
                    frame = draft(b"/").rsplit(b"\x1b[?25l\x1b[H", 1)[-1]
                    assert b"/clear" not in frame, "widely separated Tabs were treated as a double Tab"
                os.write(master, b"\t")
                frame = draft(b"/").rsplit(b"\x1b[?25l\x1b[H", 1)[-1]
                assert frame.count(b"/clear") == listed + 1, "second Tab did not list completions"
                assert b"Completions:" not in frame, "completion listing has an unnecessary heading"
                rows = [row for row in frame.split(b"\n") if b"/clear" in row]
                for row in rows:
                    row = row[row.index(b"/clear"):].split(b"\x1b[K", 1)[0]
                    assert row.startswith(b"/clear /exit /help "), ("completion separators are not single spaces", row)
                    assert b"  " not in row, ("completion row has repeated separators", row)
            os.write(master, b"h")
            frame = draft(b"/h").rsplit(b"\x1b[?25l\x1b[H", 1)[-1]
            assert b"/clear" in frame, "completion choices vanished on the next redraw"
            type_text(b"e", prefix=b"/h")
            os.write(master, b"\t")
            draft(b"/help ", point=len(b"/help "))
            os.write(master, b"x")
            draft(b"/help x")
            os.write(master, b"\x7f")
            draft(b"/help ")
            editable("completed command")
            os.write(master, b"\r")
            terminal.expect(b"> /help \x1b[K\r")
            terminal.expect(b"/regen")
            terminal.expect(b"askai | ready")
            draft(b"")
            editable("after help")
            assert not prompts, prompts

            type_text(b"hi")
            os.write(master, b"\r")
            assert arrived.wait(5), "ordinary Enter did not submit hi"
            terminal.expect(b"askai | working")
            draft(b"")
            editable("while first answer is pending")
            type_text(b"queued second")
            os.write(master, b"\r")
            draft(b"")
            editable("after queuing a line while busy")
            type_text(b"acx")
            os.write(master, b"\r")
            # One pending line already occupies the queue. A second Enter
            # must leave this next draft intact and the editor usable.
            draft(b"acx")
            editable("after Enter while busy")
            # This user binding invokes Readline's accept-line directly,
            # bypassing the CLI's Enter handler and its full-slot check.
            os.write(master, b"\x14")
            draft(b"acx")
            editable("after custom accept-line with a queued line")
            os.write(master, b"\x7f")
            draft(b"ac")
            os.write(master, b"\x1b[D")
            os.write(master, b"b")
            draft(b"abc")
            os.write(master, b"\x1b[C")
            os.write(master, b"X")
            draft(b"abcX")
            os.write(master, b"\x08")
            draft(b"abc")
            os.write(master, b"\x1bOD")
            os.write(master, b"Y")
            draft(b"abYc")
            os.write(master, b"\x1bOC")
            os.write(master, b"D")
            draft(b"abYcD")
            os.write(master, b"\x7f")
            draft(b"abYc")
            os.write(master, b"\x01h\x05")
            draft(b"habYc")
            editable("after erase and cursor keys")

            release.set()
            terminal.expect(b"keyboard-answer-one")
            terminal.expect(b"keyboard-answer-two")
            terminal.expect(b"askai | ready")
            draft(b"habYc")
            editable("after the queued answer")
            assert prompts == ["hi\n", "queued second\n"], prompts
            os.write(master, b"\r")
            terminal.expect(b"keyboard-answer-three")
            terminal.expect(b"askai | ready")
            draft(b"")
            editable("after third answer")
            assert prompts == ["hi\n", "queued second\n", "habYc\n"], prompts
            type_text(b"h")
            os.write(master, b"\x1b[5~")
            draft(b"habYc")
            os.write(master, b"\x1b[5~")
            draft(b"hi")
            os.write(master, b"\x1b[6~")
            draft(b"habYc")
            os.write(master, b"\x01\x0b")
            draft(b"")
            type_text(b"/qu")
            os.write(master, b"\t")
            draft(b"/quit ", point=len(b"/quit "))
            os.write(master, b"\r")
            assert proc.wait(timeout=5) == 0, proc.returncode
            assert termios.tcgetattr(slave) == before, "ordinary keyboard exit did not restore the terminal"
        finally:
            release.set()
            finish(proc)
            os.close(master)
            os.close(slave)
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
            master, slave = pty.openpty()
            fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 100, 0, 0))
            before = termios.tcgetattr(slave)
            inputrc = directory + "/inputrc"
            with open(inputrc, "w") as bindings:
                bindings.write('"\\C-t": "macro first\\C-mmacro second\\C-m"\n')
            environment = os.environ | {"TERM": "screen.xterm-256color", "LC_ALL": "C.UTF-8",
                                        "INPUTRC": inputrc}
            proc = subprocess.Popen([cli, "--socket", peer.socket, "--no-history", "--color", "off",
                                     "--prompt", "initial"], stdin=slave, stdout=slave,
                                    stderr=slave, env=environment)
            terminal = Terminal(proc, master)
            try:
                terminal.expect(b"\x1b[?2004h")
                assert arrived[0].wait(5), "initial request did not arrive"
                terminal.expect(b"> \x1b[K\x1b[24;")
                os.write(master, b"\x14" if macro else b"queued before eof\r")
                terminal.expect(b"1 queued")
                terminal.expect(b"> \x1b[K\x1b[24;")
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
                    terminal.expect(b"askai | ready")
                    terminal.expect(b"> \x1b[K\x1b[24;")
                    os.write(master, b"\x04")
                assert proc.wait(timeout=5) == 0, (macro, proc.returncode)
                assert prompts == expected, (macro, prompts)
                assert termios.tcgetattr(slave) == before, "queued input exit did not restore terminal settings"
            finally:
                for event in release:
                    event.set()
                finish(proc)
                os.close(master)
                os.close(slave)
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
        master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 100, 0, 0))
        before = termios.tcgetattr(slave)
        environment = os.environ | {"TERM": "screen.xterm-256color", "LC_ALL": "C.UTF-8",
                                    "INPUTRC": os.devnull}
        proc = subprocess.Popen([cli, "--socket", peer.socket, "--no-history", "--color", "off"],
                                stdin=slave, stdout=slave, stderr=slave, env=environment)
        terminal = Terminal(proc, master)

        def draft(text):
            terminal.expect(b"> " + text + b"\x1b[K\x1b[24;" + str(3 + len(text)).encode() + b"H")

        try:
            terminal.expect(b"\x1b[?2004h")
            draft(b"")
            os.write(master, b"/names\r")
            terminal.expect(b"Known session participants: rx6900xt rx9070xt")
            terminal.expect(b"askai | ready")
            draft(b"")
            for prefix, completed in ((b"@rx", b"@rx6900xt "),
                                      (b"rx", b"rx6900xt: "),
                                      (b"/tool notebook_service participant=rx9",
                                       b"/tool notebook_service participant=rx9070xt ")):
                os.write(master, prefix)
                draft(prefix)
                os.write(master, b"\t")
                draft(completed)
                os.write(master, b"x")
                draft(completed + b"x")
                os.write(master, b"\x01\x0b")
                draft(b"")
            os.write(master, b"/quit\r")
            assert proc.wait(timeout=3) == 0, proc.returncode
            assert termios.tcgetattr(slave) == before, "completion exit did not restore terminal settings"
            calls = [request for request in peer.requests if request["method"] == "tools/call"]
            assert len(calls) == 1, calls
        finally:
            finish(proc)
            os.close(master)
            os.close(slave)
    print("cli events: chat mentions use the connected GPU while notebook completion retains all participants: PASS", flush=True)


def check_fullscreen_colors(cli):
    reasoning = "reasoning-color-visible".ljust(120, ".") + "wrapped-reasoning-visible"
    answer = "answer-color-visible"

    def reply(request):
        events = [(None, {"jsonrpc": "2.0", "method": "notifications/askai/delta",
                          "params": {"requestId": request["id"], "kind": "reasoning", "text": reasoning}}),
                  (None, {"jsonrpc": "2.0", "id": request["id"], "result": {
                      "content": [{"type": "text", "text": answer}]}})]
        return 200, "text/event-stream", sse_wire(events)

    def style(frame, marker, last=False):
        assert marker in frame, (marker, frame)
        foreground, bold = None, False
        position = frame.rindex(marker) if last else frame.index(marker)
        for match in re.finditer(rb"\x1b\[([0-9;]*)m", frame[:position]):
            for code in (int(value or b"0") for value in match[1].split(b";")):
                if code == 0:
                    foreground, bold = None, False
                elif code == 1:
                    bold = True
                elif code == 22:
                    bold = False
                elif 30 <= code <= 37 or 90 <= code <= 97:
                    foreground = code
                elif code == 39:
                    foreground = None
        return foreground, bold

    for color, no_color in (("on", False), ("off", False), ("auto", True)):
        with Peer(reply) as peer:
            master, slave = pty.openpty()
            fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 100, 0, 0))
            before = termios.tcgetattr(slave)
            environment = os.environ | {"TERM": "screen.xterm-256color", "LC_ALL": "C.UTF-8",
                                        "INPUTRC": os.devnull}
            environment.pop("NO_COLOR", None)
            if no_color:
                environment["NO_COLOR"] = "1"
            proc = subprocess.Popen([cli, "--socket", peer.socket, "--no-history", "--color", color,
                                     "--prompt", "color request"], stdin=slave, stdout=slave,
                                    stderr=slave, env=environment)
            terminal = Terminal(proc, master)

            def frame(marker):
                output = terminal.expect(marker) + terminal.expect(b"\x1b[?25h")
                return output.rsplit(b"\x1b[?25l\x1b[H", 1)[-1]

            def check(output, draft=False):
                if color == "on":
                    assert style(output, b"Connected to ask_ai.")[0] == 35, output
                    assert style(output, b"reasoning-color-visible")[0] == 90, output
                    assert style(output, b"wrapped-reasoning-visible")[0] == 90, output
                    assert style(output, answer.encode())[0] is None, output
                    if draft:
                        assert style(output, b"Error:") == (31, True), output
                        assert style(output, b"draft-color-visible") == (32, True), output
                        assert style(output, b"> ", last=True)[0] == 33, output
                else:
                    codes = [int(value or b"0") for match in re.finditer(rb"\x1b\[([0-9;]*)m", output)
                             for value in match[1].split(b";")]
                    assert not any(30 <= code <= 37 or 90 <= code <= 97 or code in (38, 48)
                                   for code in codes), (color, no_color, output)

            try:
                initial = terminal.expect(answer.encode())
                initial += terminal.expect(b"askai | ready") + terminal.expect(b"\x1b[?25h")
                check(initial.rsplit(b"\x1b[?25l\x1b[H", 1)[-1])
                os.write(master, b"/tools absent\r")
                terminal.expect(b"No accessible tool named absent")
                frame(b"askai | ready")
                os.write(master, b"draft-color-visible")
                check(frame(b"draft-color-visible"), draft=True)
                fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 60, 0, 0))
                proc.send_signal(signal.SIGWINCH)
                check(frame(b"\x1b[?25l\x1b[H"), draft=True)
                os.write(master, b"\x01\x0b/quit\r")
                assert proc.wait(timeout=3) == 0, (color, no_color, proc.returncode)
                assert termios.tcgetattr(slave) == before, "color exit did not restore terminal settings"
            finally:
                finish(proc)
                os.close(master)
                os.close(slave)
    print("cli events: full-screen colors survive redraw and obey --color off and NO_COLOR: PASS", flush=True)


def check_terminal_cells(cli):
    tmux = shutil.which("tmux")
    if not tmux:
        print("cli events: terminal-cell wrapping: SKIP (tmux unavailable)", flush=True)
        return
    lines = ["A" * 79 + "Z", "B" * 79 + "YX", "界" * 40, "\t" + "C" * 71 + "Q"]
    answer = "\n".join(lines + ["cells-done"])
    with Peer(lambda request: response(answer)) as peer, \
            tempfile.TemporaryDirectory(prefix="askai-cli-cells-") as directory:
        socket = directory + "/tmux.sock"
        pane = "cells:0.0"
        environment = os.environ.copy()
        environment.pop("TMUX", None)
        environment.pop("TMUX_PANE", None)

        def run(*arguments, check=True):
            return subprocess.run([tmux, "-u", "-S", socket, "-f", os.devnull, *arguments],
                                  env=environment, capture_output=True, text=True, timeout=3, check=check)

        def screen(marker):
            deadline = time.monotonic() + 5
            while True:
                output = run("capture-pane", "-p", "-t", pane).stdout
                if marker in output and "askai | ready" in output:
                    return output
                assert time.monotonic() < deadline, ("terminal did not redraw", marker, output)
                time.sleep(0.02)

        try:
            # Use a private server and configure the pane before starting the
            # CLI; never attach to or change the user's tmux server.
            run("new-session", "-d", "-s", "cells", "-x", "80", "-y", "24", "sleep 30")
            run("set-option", "-g", "status", "off")
            run("set-option", "-w", "-t", "cells:0", "remain-on-exit", "on")
            run("set-option", "-w", "-t", "cells:0", "window-size", "manual")
            command_line = shlex.join(["env", "TERM=screen.xterm-256color", "LC_ALL=C.UTF-8",
                                       "INPUTRC=" + os.devnull, cli, "--socket", peer.socket,
                                       "--no-history", "--color", "off", "--prompt", "wrap cells"])
            run("respawn-pane", "-k", "-t", pane, command_line)
            for columns in (80, 40):
                if columns == 40:
                    run("resize-window", "-t", "cells:0", "-x", str(columns), "-y", "24")
                    run("send-keys", "-t", pane, "-l", "resize-marker")
                output = screen("resize-marker" if columns == 40 else "cells-done")
                expected = []
                for text in lines:
                    if text.startswith("界"):
                        expected.extend(text[i:i + columns // 2] for i in range(0, len(text), columns // 2))
                    else:
                        text = text.expandtabs(8)
                        expected.extend(text[i:i + columns] for i in range(0, len(text), columns))
                expected.append("cells-done")
                assert "\n".join(expected) in output, (columns, "rightmost transcript cells were lost", output)
                status = " askai | ready | 0 unread | PgUp/PgDn scroll, Ctrl-End follow"
                assert status[:columns] in output.splitlines(), (columns, "rightmost status cell was lost", output)
            run("send-keys", "-t", pane, "C-a", "C-k")
            run("send-keys", "-t", pane, "-l", "/quit")
            run("send-keys", "-t", pane, "Enter")
            deadline = time.monotonic() + 3
            while True:
                status = run("display-message", "-p", "-t", pane, "#{pane_dead}:#{pane_dead_status}").stdout.strip()
                if status.startswith("1:"):
                    assert status == "1:0", status
                    break
                assert time.monotonic() < deadline, ("CLI did not quit", status)
                time.sleep(0.02)
        finally:
            run("kill-server", check=False)
    print("cli events: real terminal cells preserve full-width ASCII, wide text and tabs through resize: PASS", flush=True)


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
        master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 100, 0, 0))
        before = termios.tcgetattr(slave)
        environment = os.environ | {"TERM": "screen.xterm-256color", "LC_ALL": "C.UTF-8",
                                    "INPUTRC": os.devnull}
        proc = subprocess.Popen([cli, "--socket", peer.socket, "--no-history", "--color", "off",
                                 "--prompt", "history-seed"], stdin=slave, stdout=slave,
                                stderr=slave, env=environment)
        terminal = Terminal(proc, master)
        try:
            terminal.expect(b"history-answer-visible")
            terminal.expect(b"askai | ready")
            terminal.expect(b"> \x1b[K\x1b[24;")
            os.write(master, b"held-generation\r")
            assert arrived.wait(5), "generation did not start"
            terminal.expect(b"askai | working")
            terminal.expect(b"> \x1b[K\x1b[24;")
            requests = len(peer.requests)
            for queued in (False, True):
                if queued:
                    os.write(master, b"queued-prompt-preserved\r")
                    terminal.expect(b"1 queued")
                    terminal.expect(b"> \x1b[K\x1b[24;")
                for name, marker in ((b"/help", b"/regen"),
                                     (b"/history", b"history-answer-visible"),
                                     (b"/names", b"cached-person"),
                                     (b"/tools", b"cached-tool-description")):
                    # A distinct second-round echo avoids matching the same
                    # command's older output while the screen is redrawn.
                    entered = b" " * queued + name
                    os.write(master, entered + b"\r")
                    terminal.expect(b"> " + entered + b"\x1b[K\r", timeout=2)
                    terminal.expect(marker, timeout=2)
                    terminal.expect(b"> \x1b[K\x1b[24;", timeout=2)
                    assert proc.poll() is None, (name, proc.returncode)
                    flags = termios.tcgetattr(slave)[3]
                    assert not flags & (termios.ICANON | termios.ECHO), (name, flags)
                    assert len(peer.requests) == requests, (name, peer.requests[requests:])
                    assert prompts == ["history-seed", "held-generation\n"], prompts
            release.set()
            terminal.expect(b"held-answer-finished")
            terminal.expect(b"queued-answer-finished")
            terminal.expect(b"askai | ready")
            terminal.expect(b"> \x1b[K\x1b[24;")
            assert prompts == ["history-seed", "held-generation\n", "queued-prompt-preserved\n"], prompts
            os.write(master, b"/quit\r")
            assert proc.wait(timeout=3) == 0, proc.returncode
            assert termios.tcgetattr(slave) == before, "local-command exit did not restore terminal settings"
        finally:
            release.set()
            finish(proc)
            os.close(master)
            os.close(slave)
    print("cli events: local commands run during generation and preserve a queued prompt: PASS", flush=True)


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
            master, slave = pty.openpty()
            fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 100, 0, 0))
            before = termios.tcgetattr(slave)
            environment = os.environ | {"TERM": "screen.xterm-256color", "LC_ALL": "C.UTF-8",
                                        "INPUTRC": os.devnull}
            proc = subprocess.Popen([cli, "--socket", peer.socket, "--no-history", "--color", "off",
                                     "--prompt", "held-generation"], stdin=slave, stdout=slave,
                                    stderr=slave, env=environment)
            terminal = Terminal(proc, master)
            try:
                terminal.expect(b"\x1b[?2004h")
                assert arrived.wait(5), "generation did not start"
                terminal.expect(b"> \x1b[K\x1b[24;")
                os.write(master, b"queued-prompt\r")
                terminal.expect(b"1 queued")
                terminal.expect(b"> \x1b[K\x1b[24;")
                started = time.monotonic()
                os.write(master, name + b"\r")
                assert proc.wait(timeout=3) == 0, (name, proc.returncode)
                assert time.monotonic() - started < 3, (name, "quit waited for generation")
                assert termios.tcgetattr(slave) == before, (name, "terminal settings were not restored")
                calls = [request for request in peer.requests if request["method"] == "tools/call"]
                assert len(calls) == 1, (name, calls)
            finally:
                release.set()
                finish(proc)
                os.close(master)
                os.close(slave)
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
            master, slave = pty.openpty()
            fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 100, 0, 0))
            before = termios.tcgetattr(slave)
            environment = os.environ | {"TERM": "screen.xterm-256color", "LC_ALL": "C.UTF-8",
                                        "INPUTRC": os.devnull}
            proc = subprocess.Popen([cli, "--socket", peer.socket, "--no-history", "--color", "off",
                                     "--prompt", "first-seed"], stdin=slave, stdout=slave,
                                    stderr=slave, env=environment)
            terminal = Terminal(proc, master)
            try:
                for marker, entered in ((b"first-seed-answer", b"second-seed\r"),
                                        (b"second-seed-answer", b"held-turn\r")):
                    terminal.expect(marker)
                    terminal.expect(b"askai | ready")
                    terminal.expect(b"> \x1b[K\x1b[24;")
                    os.write(master, entered)
                assert arrived.wait(5), "generation did not start"
                terminal.expect(b"askai | working")
                terminal.expect(b"> \x1b[K\x1b[24;")
                if name == b"/clear":
                    os.write(master, b"after-clear\r")
                    terminal.expect(b"1 queued")
                    terminal.expect(b"> \x1b[K\x1b[24;")
                os.write(master, name + b"\r")
                assert cancelled.wait(2), (name, "active generation was not cancelled")
                if name == b"/clear":
                    terminal.expect(b"Conversation cleared.", timeout=2)
                terminal.expect(b"replacement-answer", timeout=2)
                terminal.expect(b"askai | ready")
                terminal.expect(b"> \x1b[K\x1b[24;")
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
                    terminal.expect(b"askai | ready")
                    terminal.expect(b"> \x1b[K\x1b[24;")
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
                finish(proc)
                os.close(master)
                os.close(slave)
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
        raise SystemExit("usage: cli_events.py ASKAI_CLI [sticky lock saved-lock fds exhaust tui partial keys queue mentions colors cells commands quit interrupt files slow worker ...]")
    cli = os.path.realpath(sys.argv[1])
    checks = {"sticky": check_sticky_termination, "lock": check_history_lock,
              "saved-lock": check_saved_command_lock,
              "fds": check_descriptor_limit, "exhaust": check_descriptor_exhaustion,
              "tui": check_tui_drafts, "partial": check_partial_paste, "keys": check_keyboard,
              "queue": check_queued_input, "mentions": check_chat_completion, "colors": check_fullscreen_colors,
              "cells": check_terminal_cells, "commands": check_busy_commands, "quit": check_busy_quit,
              "interrupt": check_interrupt_commands, "files": check_regular_files,
              "slow": check_slow_output, "worker": check_blocked_worker}
    selected = sys.argv[2:] or checks
    for name in selected:
        if name not in checks:
            raise SystemExit(f"unknown check: {name}")
        checks[name](cli)
