override THIS_DIR := $(dir $(realpath $(lastword $(MAKEFILE_LIST))))
override THESE_MK := $(wildcard $(THIS_DIR)*.mk)

ifdef THESE_MK

# Init per-target vars before importing the make fragments.
$(foreach x,$(basename $(notdir $(THESE_MK))), \
  $(eval override INC_$x :=)                   \
  $(eval override LIB_$x :=)                   \
  $(eval override SRC_$x :=)                   \
  $(eval override CFLAGS_$x :=)                \
  $(eval override CXXFLAGS_$x :=)              \
  $(eval override LDFLAGS_$x :=)               )

override BIN := $(basename $(filter-out %.c.mk,$(notdir $(THESE_MK))))

include $(THESE_MK)
include $(THIS_DIR)../mk/common.mk

endif
