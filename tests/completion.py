#!/usr/bin/env python3
"""Check the targets that bash tab completion offers for make.

For `make <TAB><TAB>`, bash-completion runs

    make -npq __BASH_MAKE_COMPLETION__=1 -C . .DEFAULT

and reads the "Files" section of the data base that make prints. It offers
every explicit target whose name starts with a letter or a digit. This check
runs the same command in the repository root, in src/ and in tests/, extracts
the targets by the same rules, and compares them with EXPECTED.
"""

import os
import re
import subprocess
import sys

# The targets that completion must offer in src/ and in tests/. It must offer
# no other target. When a makefile gains or loses a target that a person is
# meant to type, change these lists to match.
SRC = """
    all
    askai clean-askai purge-askai
    godbolt.so clean-godbolt.so purge-godbolt.so
    test_plugin.so clean-test_plugin.so purge-test_plugin.so
    twinception clean-twinception purge-twinception
    compile_commands.json clean-compile_commands.json
    clean purge
""".split()

TESTS = """
    default
    check check-askai check-buf check-completion check-dstr check-handle
    check-instance check-ledger check-load check-turn check-twinception
    load load-build
""".split()

# The expected targets by directory. The root makefile imports the targets of
# the makefiles in src/ and in tests/, so the root must offer both lists.
EXPECTED = {".": SRC + TESTS, "src": SRC, "tests": TESTS}

# Variables that a running make exports to its recipes. A shell does not have
# them, so the check removes them before it runs make.
MAKE_ENV = ("MAKEFLAGS", "MFLAGS", "MAKELEVEL", "MAKE_TERMOUT", "MAKE_TERMERR")


def database(root):
    """Return what make prints for the command that bash-completion runs."""
    env = {k: v for k, v in os.environ.items() if k not in MAKE_ENV}
    env["LC_ALL"] = "C"
    # make exits with status 2 because .DEFAULT has no rule. bash-completion
    # ignores the status and discards stderr.
    run = subprocess.run(
        ["make", "-npq", "__BASH_MAKE_COMPLETION__=1", "-C", ".", ".DEFAULT"],
        cwd=root, env=env, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
        text=True)
    return run.stdout


def offered(db):
    """Return the targets that bash-completion extracts from the data base."""
    files = re.search(r"^# +Files\n(.*)^# +Finished Make data base", db,
                      re.M | re.S)
    assert files, "make printed no data base"
    names = []
    # An empty line ends the description of one file. A block that starts with
    # "# Not a target:" does not match, because its first character is "#".
    for block in files[1].split("\n\n"):
        block = block.strip("\n")
        name = re.match(r"([A-Za-z0-9][^#\t:%]*):", block)
        if name and "File is an intermediate prerequisite" not in block:
            names.append(name[1])
    return names


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "."
    failed = False
    for sub, expected in EXPECTED.items():
        where = "the repository root" if sub == "." else sub + "/"
        got = offered(database(os.path.join(root, sub)))
        assert len(got) == len(set(got)), got
        for name in sorted(set(got) - set(expected)):
            print(f"make tab completion in {where} offers a target that is "
                  f"not expected: {name}")
            failed = True
        for name in sorted(set(expected) - set(got)):
            print(f"make tab completion in {where} does not offer an expected "
                  f"target: {name}")
            failed = True
    if failed:
        return 1

    print("make tab completion in the repository root, src/ and tests/: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
