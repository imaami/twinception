override TEST_DIR := $(dir $(realpath $(lastword $(MAKEFILE_LIST))))

include $(TEST_DIR)../mk/o.mk
$(call set-build-dir,$(TEST_DIR)../build)

.PHONY: default check check-askai check-buf check-dstr check-handle \
        check-load check-twinception load load-build \
        build-askai build-test_plugin.so build-twinception
default:; @:
check: | check-twinception check-askai check-buf check-handle check-dstr \
         check-load

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

override SRC_DIR    := $(TEST_DIR)../src/
override TEST_FLAGS := -std=gnu23 -O2 -g -Wall -Wextra -Wpedantic

# Each check builds its binary with a sub-make, and the binaries share
# objects: two sub-makes at once would compile the same files.
.NOTPARALLEL:

# What src/ builds that the checks run, a goal per make.
build-askai build-test_plugin.so build-twinception:
	+$(MAKE) -C $(TEST_DIR)../src $(@:build-%=%) O="$O"

check-twinception: | build-twinception
	$(PYTHON) $(TEST_DIR)check.py $Otwinception

# The test plugin is built, though nothing loads it yet.
check-askai: | build-askai build-test_plugin.so
	$(PYTHON) $(TEST_DIR)askai.py $Oaskai

# Unit tests of the byte buffer, src/buf.c.
check-buf: $Otests/buf
	$<

$Otests/buf: $(TEST_DIR)buf.c $(SRC_DIR)buf.c $(SRC_DIR)buf.h | $Otests
	$(CC) $(TEST_FLAGS) -I$(SRC_DIR) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) \
	      -o "$@" $(filter %.c,$^)

# Unit tests of the event loop's handles, which include src/loop.c to reach
# its slot table.
check-handle: $Otests/handle
	$<

$Otests/handle: $(TEST_DIR)handle.c $(SRC_DIR)loop.c $(SRC_DIR)loop.h \
                $(SRC_DIR)log.c | $Otests
	$(CC) $(TEST_FLAGS) -pthread -I$(SRC_DIR) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) \
	      -o "$@" $(TEST_DIR)handle.c $(SRC_DIR)log.c

# e.g. make check-dstr DSTR_CHECKFLAGS='-t gcc -S address'
check-dstr:
	$(TEST_DIR)dstr/run.sh -b $Otests/dstr $(DSTR_CHECKFLAGS)

# Load harness (tests/load): an open-loop probe, a C upstream mock and a
# profile driver; the histogram is src/hist.h.
override LOAD       := $Otests/load
override LOAD_SRC   := $(TEST_DIR)load
override LOAD_FLAGS := $(TEST_FLAGS) -pthread -I$(SRC_DIR)

load-build: | $(LOAD)/gen $(LOAD)/mock

$(LOAD)/gen: $(LOAD_SRC)/gen.c $(SRC_DIR)hist.h | $(LOAD)
	$(CC) $(LOAD_FLAGS) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o "$@" $< -lssl \
	      -lcrypto -lm

$(LOAD)/mock: $(LOAD_SRC)/mock.c | $(LOAD)
	$(CC) $(LOAD_FLAGS) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o "$@" $<

$Otests $(LOAD):; mkdir -p "$@"

# The harness's verdict (no errors, lockouts, leaks, unfairness or unbounded
# work; latency only against the same run's idle phase) on a light mix of
# every mode against $O/askai, in about 15 s.
check-load: | load-build build-askai
	BASE=2 $(LOAD_SRC)/run.sh check $Oaskai "$(LOAD)" 5

# Build the harness and askai, then run the storm profile against $O/askai.
# Override the seconds with LOAD_SECONDS and the probe rate with RATE.
load: | load-build build-askai
	RATE=$(RATE) $(LOAD_SRC)/run.sh storm $Oaskai "$(LOAD)" $(LOAD_SECONDS)

endif
