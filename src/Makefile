override THIS_DIR := $(dir $(realpath $(lastword $(MAKEFILE_LIST))))
override THESE_MK := $(wildcard $(THIS_DIR)*.mk)
ifdef THESE_MK
override BIN := $(basename $(filter-out %.c.mk,$(notdir $(THESE_MK))))
include $(THESE_MK)
include $(THIS_DIR)../mk/common.mk
endif
