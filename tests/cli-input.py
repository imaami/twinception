#!/usr/bin/env python3
"""Exercise the actual console on a PTY without needing an AF_UNIX listener."""

import errno
import fcntl
import os
from pathlib import Path
import select
import signal
import struct
import subprocess
import sys
import tempfile
import termios
import time


UP = b"\x1b[A"
DOWN = b"\x1b[B"
LEFT = b"\x1b[D"
PGUP = b"\x1b[5~"
PGDN = b"\x1b[6~"


class Console:
    """Keep machine-readable results separate from Readline's terminal output."""

    def __init__(self, binary, *args, inputrc="", columns=40):
        self.directory = tempfile.TemporaryDirectory(prefix="askai-input-")
        self.root = Path(self.directory.name)
        (self.root / "inputrc").write_text(inputrc)
        self.master, self.slave = os.openpty()
        fcntl.ioctl(self.slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, columns, 0, 0))
        self.original = termios.tcgetattr(self.slave)
        self.records, writer = os.pipe()
        self.output = b""
        self.pending = b""
        self.records_eof = False

        def session():
            os.setsid()
            fcntl.ioctl(0, termios.TIOCSCTTY, 0)

        environment = os.environ | {"TERM": "xterm-256color", "LC_ALL": "C.UTF-8",
                                    "HOME": str(self.root), "INPUTRC": str(self.root / "inputrc")}
        try:
            self.process = subprocess.Popen([binary, str(writer), *args], stdin=self.slave,
                                            stdout=self.slave, stderr=self.slave,
                                            pass_fds=(writer,), preexec_fn=session,
                                            cwd=self.root, env=environment)
        finally:
            os.close(writer)
        try:
            self.prompt()
        except BaseException:
            self.__exit__()
            raise

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        try:
            if self.process.poll() is None:
                self.process.kill()
            self.process.wait(timeout=5)
        finally:
            os.close(self.records)
            os.close(self.master)
            os.close(self.slave)
            self.directory.cleanup()

    def pump(self, timeout):
        descriptors = [self.master]
        if not self.records_eof:
            descriptors.append(self.records)
        ready, _, _ = select.select(descriptors, [], [], timeout)
        for descriptor in ready:
            try:
                data = os.read(descriptor, 65536)
            except OSError as error:
                if descriptor == self.master and error.errno == errno.EIO:
                    continue
                raise
            if descriptor == self.master:
                self.output += data
            elif data:
                self.pending += data
            else:
                self.records_eof = True

    def until(self, condition, timeout=5):
        deadline = time.monotonic() + timeout
        while not condition() and time.monotonic() < deadline:
            self.pump(max(0, deadline - time.monotonic()))
            if self.records_eof and self.process.poll() is not None:
                break
        assert condition(), (self.process.poll(), self.pending, self.output)

    def prompt(self):
        self.until(lambda: b"test> " in self.output)
        self.output = self.output.split(b"test> ", 1)[1]

    def send(self, data):
        if isinstance(data, str):
            data = data.encode()
        remaining = memoryview(data)
        while remaining:
            count = os.write(self.master, remaining)
            assert count > 0
            remaining = remaining[count:]

    def record(self):
        self.until(lambda: b"\n" in self.pending)
        record, self.pending = self.pending.split(b"\n", 1)
        return record

    def line(self, keys, expected):
        self.send(keys)
        record = self.record()
        assert record.startswith(b"LINE "), (record, self.output)
        actual = bytes.fromhex(record[5:].decode()).decode()
        assert actual == expected, (actual, expected, self.output)
        self.prompt()

    def finish(self, signum=None, keyboard=False):
        if keyboard:
            self.send(b"\x03")
        elif signum:
            self.process.send_signal(signum)
        else:
            self.send(b"\x04")
        expected = b"STOP " + str(int(signum)).encode().hex().encode() if signum else b"EOF"
        assert self.record() == expected, (self.pending, self.output)
        status = self.process.wait(timeout=5)
        self.pump(0)
        assert status == 0, self.output
        assert termios.tcgetattr(self.slave) == self.original, "terminal attributes were not restored"


def check_history(binary):
    with Console(binary) as console:
        for text in ("alpha first", "beta middle", "alpha second"):
            console.line(text + "\r", text + "\n")
        # The match is the prefix before point, not the unedited suffix. Point
        # stays after that prefix so typing edits the recalled line in place.
        console.line(b"alphastale" + LEFT * 5 + PGUP + PGUP + PGDN + b"!\r",
                     "alpha! second\n")
        console.line(UP + b"\x05?\r", "alpha! second?\n")
        console.line(b"unsent draft" + UP + DOWN + b"\r", "unsent draft\n")
        console.line(b"\x12beta\r", "beta middle\n")
        console.finish()
    print("cli input history: prefix PgUp/PgDn, cursor, arrows, draft and Ctrl-R: PASS")


def check_history_edits(binary):
    # Readline mutates a recalled history entry when navigation leaves it.
    # Its replacement can be shorter or longer than the submitted entry.
    with Console(binary) as console:
        console.line("newtext!\r", "newtext!\n")
        console.line("longline\r", "longline\n")
        console.line(UP + b"\x01\x0bx" + UP + b"\r", "newtext!\n")
        console.line(UP + UP + DOWN + UP + b"\r", "x\n")
        suffix = b" plus a considerably longer suffix"
        console.line(UP + b"\x05" + suffix + UP + DOWN + b"\r", "x" + suffix.decode() + "\n")
        console.line(UP + b"\r", "x" + suffix.decode() + "\n")
        # Repeating the last submission must not append another history entry.
        console.line(UP * 3 + b"\r", "newtext!\n")
        console.finish()
    print("cli input history edits: native mutation, shorter/longer entries and deduplication: PASS")


def check_editing(binary):
    with Console(binary, columns=20) as console:
        console.line("ääλ".encode() + LEFT + b"\x7fX\r", "äXλ\n")
        text = "wrapped " + "界äλ" * 30
        console.line(text.encode() + b"\x01BEGIN " + b"\x05 END\r", "BEGIN " + text + " END\n")
        console.send(text)
        fcntl.ioctl(console.slave, termios.TIOCSWINSZ, struct.pack("HHHH", 24, 32, 0, 0))
        console.process.send_signal(signal.SIGWINCH)
        console.line(b"\x01RESIZED " + b"\x05 END\r", "RESIZED " + text + " END\n")
        console.line(b"alpha beta\x17gamma\r", "alpha gamma\n")
        console.line(b"/he\t\r", "/help\n")
        (console.root / "path with space.txt").write_text("fixture")
        console.line(b"/read path\t\r", "/read path with space.txt\n")
        (console.root / "documents").mkdir()
        console.line(b"/read doc\tchild\r", "/read documents/child\n")
        (console.root / "story.txt").write_text("fixture")
        console.line(b"/read stale.txt" + LEFT * 7 + b"\t\r", "/read story.txt\n")
        console.line(b"/read docstale/child" + LEFT * 11 + b"\t\r", "/read documents/child\n")
        console.finish()
    print("cli input editing: UTF-8, wrapping/resize, word erase and Tab: PASS")


def check_multiline(binary):
    with Console(binary) as console:
        console.line(b"first\\\rsecond\r", "first\nsecond\n")
        console.line(UP + b"\r", "first\nsecond\n")
        # Embedded paste newlines must stay in the edit buffer until Enter.
        console.send(b"\x1b[200~pasted one\npasted two\x1b[201~")
        deadline = time.monotonic() + 0.15
        while time.monotonic() < deadline:
            console.pump(max(0, deadline - time.monotonic()))
            assert not console.pending, (console.pending, console.output)
        console.line(b"\r", "pasted one\npasted two\n")
        console.line(UP + b"\r", "pasted one\npasted two\n")
        console.finish()
    with Console(binary, "--multiline") as console:
        console.line(b"first\rsecond/\r", "first\nsecond\n")
        console.line(UP + b"/\r", "first\nsecond\n")
        console.finish()
    print("cli input multiline: continuation, paste, explicit submit and whole-prompt recall: PASS")


def check_catalog(binary):
    with Console(binary, "--catalog") as console:
        console.line(b"/tool gi\tcommand=lo\t\r", "/tool git command=log \n")
        console.line(b"/tool git rec\tt\t\r", "/tool git recursive=true \n")
        console.line(b"/tool git participant=al\t\r", "/tool git participant=alice \n")
        console.line(b"al\thello\r", "alice: hello\n")
        console.line(b"hello @al\tthere\r", "hello @alice there\n")
        console.line(b"hello @al, next" + LEFT * 6 + b"\t\r", "hello @alice, next\n")
        console.line(b"al: hi" + LEFT * 4 + b"\t\r", "alice: hi\n")
        console.line("ä\t".encode() + b"hello\r", "äiti: hello\n")
        # Completing within a token replaces its stale tail, preserving later
        # arguments. Readline must honor both candidate text and cursor.
        original = b"/tool git command=stale count=2"
        console.line(original + LEFT * 10 + b"\t\r", "/tool git command=status count=2\n")
        console.line(original + LEFT * 10 + b"\t\x1f\r", original.decode() + "\n")
        console.line(b"/tool absent\t\r", "/tool absent\n")
        console.finish()
    with Console(binary, "--catalog", inputrc='"\\C-t": possible-completions\n') as console:
        console.line(original + LEFT * 10 + b"\x14\r", original.decode() + "\n")
        console.line(b"/unknown\t\x15" + original + LEFT * 10 + b"\x14\r",
                     original.decode() + "\n")
        console.finish()
    with Console(binary, "--catalog", inputrc='"\\C-t": menu-complete\n') as console:
        console.line(b"/tool git command=\x14\x14\r", "/tool git command=status \n")
        console.finish()
    print("cli input completion: MCP tools, typed arguments, names and token replacement: PASS")


def check_inputrc(binary):
    # A user binding must override the application's defaults, and the
    # application name must be available to inputrc's conditional syntax.
    inputrc = ('$if askai-cli\n"\\e[5~": beginning-of-line\n'
               '"\\C-t": "hello\\C-mworld\\C-m"\n$endif\n')
    with Console(binary, inputrc=inputrc) as console:
        console.line(b"old history\r", "old history\n")
        console.line(b"custom" + PGUP + b"X\r", "Xcustom\n")
        console.line(b"\x14", "hello\n")
        # The rest of a macro must execute without another physical keystroke.
        console.line(b"", "world\n")
        console.finish()
    print("cli input configuration: inputrc overrides and macros spanning submissions: PASS")


def check_signals(binary):
    for pending in (b"unfinished prompt", b"unfinished escape\x1b[",
                    b"\x1b[200~unfinished paste\n", b"unfinished UTF-8 \xc3",
                    b"\x12unfinished search"):
        for signum, keyboard in ((signal.SIGINT, True), (signal.SIGTERM, False)):
            with Console(binary) as console:
                console.send(pending)
                # Let Readline consume the unfinished key/paste/UTF-8 sequence
                # before interrupting its attempt to read the missing bytes.
                deadline = time.monotonic() + 0.05
                while time.monotonic() < deadline:
                    console.pump(max(0, deadline - time.monotonic()))
                    assert not console.pending, (console.pending, console.output)
                console.finish(signum, keyboard)
    print("cli input signals: Ctrl-C/SIGTERM unblock input and restore termios exactly: PASS")


def check_simple(binary):
    for args, entered, expected in (
            ((), b"one\r\n\ntwo", ["one\n", "\n", "two\n"]),
            (("--simple",), b"first\\\nsecond\n", ["first\nsecond\n"]),
            (("--multiline",), b"first\nsecond/\n", ["first\nsecond\n"]),
            ((), b"", [])):
        reader, writer = os.pipe()
        try:
            result = subprocess.run([binary, str(writer), *args], input=entered,
                                    capture_output=True, pass_fds=(writer,), timeout=5)
        finally:
            os.close(writer)
        try:
            records = b""
            while chunk := os.read(reader, 65536):
                records += chunk
        finally:
            os.close(reader)
        assert result.returncode == 0, result
        wanted = [b"LINE " + text.encode().hex().encode() for text in expected] + [b"EOF"]
        assert records.splitlines() == wanted, (records, wanted, result)
        assert b"\x1b[" not in result.stdout, result.stdout
    print("cli input pipes: automatic simple input, multiline and EOF: PASS")


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: cli-input.py CLI-INPUT-HARNESS")
    binary = os.path.abspath(sys.argv[1])
    for check in (check_history, check_history_edits, check_editing, check_multiline, check_catalog, check_inputrc,
                  check_signals, check_simple):
        check(binary)


if __name__ == "__main__":
    main()
