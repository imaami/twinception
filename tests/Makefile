override TEST_DIR := $(dir $(realpath $(lastword $(MAKEFILE_LIST))))

include $(TEST_DIR)../mk/o.mk
$(call set-build-dir,$(TEST_DIR)../build)

.PHONY: default check check-askai check-buf check-completion check-datauri \
        check-dstr check-handle check-instance check-ledger check-load \
        check-turn check-twinception load load-build
default:; @:
check: | check-completion check-twinception check-askai check-buf \
         check-datauri check-handle check-turn check-instance check-ledger \
         check-dstr check-load

# Nothing below this comment is defined when tab-completing.

ifneq (.DEFAULT,$(MAKECMDGOALS))

PYTHON := python3

# C tests prefer clang like the main build, but honour a command-line or
# environment CC.
ifeq (,$(filter-out default undefined,$(origin CC)))
override CC := $(shell command -v clang > /dev/null 2>&1 && echo clang || echo cc)
endif

# Command-line flags are appended, e.g. CFLAGS='-O1 -g -fsanitize=address'.
# No `override` here, it clobbers command-line variables.
CFLAGS   :=
CPPFLAGS :=
LDFLAGS  :=

override SRC_DIR    := $(realpath $(TEST_DIR)../src)/
override TEST_FLAGS := -std=gnu23 -O2 -g -Wall -Wextra -Wpedantic -pthread

# Each check builds its binary with a sub-make, and the binaries share
# objects: two sub-makes at once would compile the same files.
.NOTPARALLEL:

# Targets of the makefile in src/ that the checks need. The recipe runs a
# sub-make in src/ with the name of the target as its one goal, so any target
# that src/Makefile exports can be added to the list. These targets are for
# the rules in this makefile only. They are defined below the tab-completion
# guard, so the root makefile does not import them and tab completion does
# not offer them.
.PHONY: askai godbolt.so test_plugin.so twinception
askai godbolt.so test_plugin.so twinception:
	+$(MAKE) -C "$(TEST_DIR)../src" $@ O="$O"

# Compares the targets that bash tab completion offers for make in the
# repository root, in src/ and in tests/ with the lists in completion.py.
check-completion:
	$(PYTHON) $(TEST_DIR)completion.py $(TEST_DIR)..

check-twinception: | twinception
	$(PYTHON) $(TEST_DIR)check.py $Otwinception

# askai.py loads the test plugin in check_plugin() and the godbolt plugin in
# check_godbolt().
check-askai: | askai test_plugin.so godbolt.so
	$(PYTHON) $(TEST_DIR)askai.py $Oaskai $Otest_plugin.so $Ogodbolt.so

# Unit tests of the byte buffer, src/buf.c.
check-buf: $Otests/buf
	$<

# Unit tests of the data: URIs of files that clients hand askai,
# src/datauri.c.
check-datauri: $Otests/datauri
	$<

# Unit tests of the event loop's handles, which include src/loop.c to reach
# its slot table.
check-handle: $Otests/handle
	$<

# check-turn runs unit tests of the order in which a turn of the event loop
# runs its steps.  The test includes src/loop.c and replaces clock_gettime()
# in it with a fake clock.
check-turn: $Otests/turn
	$<

# Unit tests of the table of plugin instances.  The test includes
# src/loop.c and src/instance.c, so that it can call their static functions.
check-instance: $Otests/instance
	$<

# Unit tests of the ledger of tasks and jobs.  The test includes src/loop.c
# and src/ledger.c, so that it can read the ledger's static structures, and
# replaces clock_gettime() in them with a fake clock.
check-ledger: $Otests/ledger
	$<

# The unit tests.  Each test links the object of its own source and the
# objects of the files of src/ that the lines below list.
override UNIT := buf datauri handle instance ledger turn

$Otests/buf: $Otests/src/buf.c.o
$Otests/datauri: $Otests/src/datauri.c.o
$Otests/handle $Otests/instance $Otests/ledger $Otests/turn: $Otests/src/log.c.o

$(UNIT:%=$Otests/%): $Otests/%: $Otests/%.c.o
	$(CC) $(TEST_FLAGS) $(CFLAGS) $(LDFLAGS) -o "$@" $^

# Each source is compiled on its own, as in mk/common.mk.  -MMD writes a
# rule to a .d file beside the object.  The rule lists every file that the
# source includes, directly or through other files, as a prerequisite of
# the object.  The .d files are included at the end of this makefile, so a
# change of an included file rebuilds only the objects that include it.
$Otests/%.c.o: $(TEST_DIR)%.c | $Otests
	$(CC) $(TEST_FLAGS) -I$(SRC_DIR) $(CPPFLAGS) $(CFLAGS) -MMD -c -o "$@" "$<"

$Otests/src/%.c.o: $(SRC_DIR)%.c | $Otests/src
	$(CC) $(TEST_FLAGS) -I$(SRC_DIR) $(CPPFLAGS) $(CFLAGS) -MMD -c -o "$@" "$<"

# e.g. make check-dstr DSTR_CHECKFLAGS='-t gcc -S address'
check-dstr:
	$(TEST_DIR)dstr/run.sh -b $Otests/dstr $(DSTR_CHECKFLAGS)

# Load harness (tests/load): an open-loop probe, a C upstream mock and a
# profile driver; the histogram is src/hist.h.
override LOAD       := $Otests/load
override LOAD_SRC   := $(TEST_DIR)load
override LOAD_FLAGS := $(TEST_FLAGS) -I$(SRC_DIR)

load-build: | $(LOAD)/gen $(LOAD)/mock

# Each program of the harness has one source, so one command compiles and
# links it, and -MMD writes the rule for the program to a .d file.
$(LOAD)/gen: $(LOAD_SRC)/gen.c | $(LOAD)
	$(CC) $(LOAD_FLAGS) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -MMD -o "$@" $< -lssl \
	      -lcrypto -lm

$(LOAD)/mock: $(LOAD_SRC)/mock.c | $(LOAD)
	$(CC) $(LOAD_FLAGS) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -MMD -o "$@" $<

$Otests $Otests/src $(LOAD):; mkdir -p "$@"

# The harness's verdict (no errors, lockouts, leaks, unfairness or unbounded
# work; latency only against the same run's idle phase) on a light mix of
# every mode against $O/askai, in about 15 s.
check-load: | load-build askai
	BASE=2 $(LOAD_SRC)/run.sh check $Oaskai "$(LOAD)" 5

# Build the harness and askai, then run the storm profile against $O/askai.
# Override the seconds with LOAD_SECONDS and the probe rate with RATE.
load: | load-build askai
	RATE=$(RATE) $(LOAD_SRC)/run.sh storm $Oaskai "$(LOAD)" $(LOAD_SECONDS)

-include $(UNIT:%=$Otests/%.c.d) $Otests/src/buf.c.d $Otests/src/datauri.c.d \
         $Otests/src/log.c.d \
         $(LOAD)/gen.d $(LOAD)/mock.d

endif
