override TEST_DIR := $(dir $(realpath $(lastword $(MAKEFILE_LIST))))

include $(TEST_DIR)../mk/o.mk
$(call set-build-dir,$(TEST_DIR)../build)

.PHONY: default check check-askai check-buf check-cli check-completion check-cstr-view check-datauri \
        check-dstr check-handle check-instance check-journal check-ledger check-load \
        check-turn check-twinception load load-build
default:; @:
check: | check-completion check-twinception check-askai check-cli check-buf \
         check-datauri check-handle check-turn check-instance check-journal check-ledger \
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
PKG_CONFIG := pkg-config

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
.PHONY: askai askai-cli godbolt.so test_plugin.so twinception
askai askai-cli godbolt.so test_plugin.so twinception:
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

# Terminal editing, the Unix-socket CLI, and its argument handling.
check-cli: $Otests/cli_io $Otests/cli_loop $Otests/cli_scrollback $Otests/cli_input \
           $Otests/cli_state $Otests/cli_ui $Otests/autocomplete | check-cstr-view askai-cli askai
check-cli:; $Otests/cli_loop && $Otests/cli_io && $Otests/cli_scrollback
	$Otests/autocomplete
	$Otests/cli_ui
	@directory=$$(mktemp -d) || exit 1; \
	trap 'rm -rf "$$directory"' EXIT; \
	"$Otests/cli_state" "$$directory/history"
	$(PYTHON) $(TEST_DIR)cli-input.py $Otests/cli_input && $(PYTHON) $(TEST_DIR)cli_events.py $Oaskai-cli
	$(PYTHON) $(TEST_DIR)chat-history.py $Oaskai
	$(PYTHON) $(TEST_DIR)chat-stream.py $Oaskai
	$(PYTHON) $(TEST_DIR)cli.py $Oaskai-cli $Oaskai

# Readline and the completion catalog are tested without a server so these
# checks also run where Unix sockets are prohibited. Keep their objects separate.
# Query each set of flags once, only when a recipe needs it, and fail there.
# Expand the shell call after eval parses the assignment, so its output is
# never parsed as makefile text or expanded again for dollar references.
override test-pkg-config = $(eval override $1 := $$(shell $$(PKG_CONFIG) $2))\
                          $(if $(filter-out 0,$(.SHELLSTATUS)),$(error $(PKG_CONFIG) $2 failed for $@))$($1)
override CLI_INCLUDES = $(call test-pkg-config,CLI_INCLUDES,--cflags readline)
override CLI_LIBS = $(call test-pkg-config,CLI_LIBS,--libs readline)
override JSON_INCLUDES = $(call test-pkg-config,JSON_INCLUDES,--cflags json-c)
override JSON_LIBS = $(call test-pkg-config,JSON_LIBS,--libs json-c)
override UTF8_INCLUDES = $(call test-pkg-config,UTF8_INCLUDES,--cflags libutf8proc)
override UTF8_LIBS = $(call test-pkg-config,UTF8_LIBS,--libs libutf8proc)
override AUTOCOMPLETE_OBJECTS := $Otests/autocomplete_fixture.c.o \
                                 $Otests/src/autocomplete.c.o $Otests/src/cli_command.c.o \
                                 $Otests/src/dstr.c.o $Otests/src/jb.c.o
override AUTOCOMPLETE_WRAPS := -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free

override CLI_CONSOLE_OBJECTS := $Otests/src/cli_console.c.o $Otests/src/buf.c.o \
                                $Otests/src/cli_loop.c.o $Otests/src/cli_mailbox.c.o \
                                $Otests/src/cli_render.c.o $Otests/src/cli_scrollback.c.o $Otests/src/cli_style.c.o $Otests/src/cli_io.c.o \
                                $Otests/src/blocker.c.o $Otests/src/loop.c.o $Otests/src/log.c.o

$Otests/cli_io: $Otests/cli_io.c.o $Otests/src/cli_io.c.o $Otests/src/cli_loop.c.o \
                $Otests/src/cli_mailbox.c.o $Otests/src/blocker.c.o $Otests/src/loop.c.o $Otests/src/log.c.o
$Otests/cli_io:; $(CC) $(TEST_FLAGS) $(CFLAGS) $(LDFLAGS) -o "$@" $^ -lm

$Otests/cli_loop: $Otests/cli_loop.c.o $Otests/src/cli_loop.c.o $Otests/src/cli_mailbox.c.o \
                  $Otests/src/loop.c.o $Otests/src/log.c.o
$Otests/cli_loop:; $(CC) $(TEST_FLAGS) $(CFLAGS) $(LDFLAGS) -o "$@" $^ -lm

$Otests/cli_scrollback: $Otests/cli_scrollback.c.o $Otests/src/cli_render.c.o $Otests/src/cli_scrollback.c.o $Otests/src/cli_style.c.o \
                       $Otests/src/dstr.c.o $Otests/src/cstr_view.c.o
$Otests/cli_scrollback:; $(CC) $(TEST_FLAGS) $(CFLAGS) $(LDFLAGS) -o "$@" $^ $(UTF8_LIBS)

$Otests/cli_input $Otests/cli_ui $Otests/cli_state $Otests/autocomplete $Otests/cstr_view: $Otests/src/cstr_view.c.o

$Otests/cstr_view: $Otests/cstr_view.c.o $Otests/src/dstr.c.o
$Otests/cstr_view:; $(CC) $(TEST_FLAGS) $(CFLAGS) $(LDFLAGS) -o "$@" $^

check-cstr-view: $Otests/cstr_view; $<

$Otests/cli_input: $Otests/cli_input.c.o $(CLI_CONSOLE_OBJECTS) \
                   $Otests/src/autocomplete_file.c.o $(AUTOCOMPLETE_OBJECTS)
$Otests/cli_input:; $(CC) $(TEST_FLAGS) $(CFLAGS) $(LDFLAGS) -o "$@" $^ $(CLI_LIBS) $(JSON_LIBS) $(UTF8_LIBS) -lm

$Otests/cli_ui: $Otests/cli_ui.c.o $Otests/src/cli_ui.c.o $(CLI_CONSOLE_OBJECTS) \
                $Otests/src/dstr.c.o $Otests/src/jb.c.o
$Otests/cli_ui:; $(CC) $(TEST_FLAGS) $(CFLAGS) $(LDFLAGS) -o "$@" $^ $(CLI_LIBS) $(JSON_LIBS) $(UTF8_LIBS) -lm

$Otests/cli_state: $Otests/cli_state.c.o $Otests/src/cli_state.c.o \
                   $Otests/src/dstr.c.o $Otests/src/jb.c.o $Otests/src/child.c.o \
                   $Otests/src/buf.c.o $Otests/src/loop.c.o $Otests/src/log.c.o
	$(CC) $(TEST_FLAGS) $(CFLAGS) $(LDFLAGS) -o "$@" $^ $(JSON_LIBS)

$Otests/autocomplete: $Otests/autocomplete.c.o $Otests/src/autocomplete_file.c.o $(AUTOCOMPLETE_OBJECTS)
	$(CC) $(TEST_FLAGS) $(CFLAGS) $(LDFLAGS) $(AUTOCOMPLETE_WRAPS) -o "$@" $^ $(JSON_LIBS) -lm

$Otests/autocomplete.c.o $Otests/autocomplete_fixture.c.o \
$Otests/src/autocomplete.c.o $Otests/src/autocomplete_file.c.o \
$Otests/src/cli_state.c.o $Otests/src/cli_console.c.o $Otests/src/jb.c.o: CPPFLAGS += $(JSON_INCLUDES)

$Otests/src/cli_console.c.o: CPPFLAGS += $(CLI_INCLUDES)
$Otests/src/cli_render.c.o: CPPFLAGS += $(UTF8_INCLUDES)

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

check-journal: $Otests/journal
	@directory=$$(mktemp -d) || exit 1; \
	trap 'rm -rf "$$directory"' EXIT; \
	"$<" "$$directory/transcripts.git"

$Otests/journal: $Otests/journal.c.o $Otests/src/loop.c.o $Otests/src/log.c.o \
                 $Otests/src/child.c.o $Otests/src/buf.c.o
$Otests/journal:; $(CC) $(TEST_FLAGS) $(CFLAGS) $(LDFLAGS) -o "$@" $^ -lcrypto -lm

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

-include $(UNIT:%=$Otests/%.c.d) $Otests/journal.c.d $Otests/src/buf.c.d $Otests/src/datauri.c.d \
         $Otests/src/log.c.d $Otests/cli_input.c.d $Otests/cli_ui.c.d $Otests/autocomplete.c.d \
         $Otests/cli_state.c.d $Otests/src/cli_state.c.d \
         $Otests/cli_loop.c.d $Otests/cli_io.c.d $Otests/cli_scrollback.c.d \
         $Otests/src/cli_loop.c.d $Otests/src/cli_io.c.d $Otests/src/cli_mailbox.c.d \
         $Otests/src/cli_render.c.d $Otests/src/cli_scrollback.c.d $Otests/src/cli_style.c.d $Otests/src/blocker.c.d $Otests/src/loop.c.d \
         $Otests/autocomplete_fixture.c.d $Otests/src/cli_console.c.d \
         $Otests/src/autocomplete.c.d $Otests/src/autocomplete_file.c.d $Otests/src/cli_command.c.d \
         $Otests/src/dstr.c.d $Otests/src/child.c.d \
         $Otests/src/cli_ui.c.d $Otests/src/jb.c.d \
         $Otests/cstr_view.c.d $Otests/src/cstr_view.c.d \
         $(LOAD)/gen.d $(LOAD)/mock.d

endif
