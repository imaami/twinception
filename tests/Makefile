override TEST_DIR := $(dir $(realpath $(lastword $(MAKEFILE_LIST))))

include $(TEST_DIR)../mk/o.mk
$(call set-build-dir,$(TEST_DIR)../build)

.PHONY: default check check-askai check-dstr check-twinception
default:; @:
check: | check-twinception check-askai check-dstr

# Nothing below this comment is defined when tab-completing.

ifneq (.DEFAULT,$(MAKECMDGOALS))

PYTHON := python3

# Each check builds its binary with a sub-make, and the binaries share
# objects: two sub-makes at once would compile the same files.
.NOTPARALLEL:

check-twinception:
	+$(MAKE) -C $(TEST_DIR)../src $(@:check-%=%) O="$O"
	$(PYTHON) $(TEST_DIR)check.py $(@:check-%=$O%)

check-askai:
	+$(MAKE) -C $(TEST_DIR)../src $(@:check-%=%) O="$O"
	$(PYTHON) $(TEST_DIR)askai.py $(@:check-%=$O%)

# e.g. make check-dstr DSTR_CHECKFLAGS='-t gcc -S address'
check-dstr:
	$(TEST_DIR)dstr/run.sh -b $Otests/dstr $(DSTR_CHECKFLAGS)

endif
