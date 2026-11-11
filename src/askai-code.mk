# askai-code, the helper program of the code tool, links libclang 19 or
# later. The build uses the llvm-config in LLVM_CONFIG if it is set on the
# command line or in the environment, or else the first of llvm-config and
# llvm-config-30 to llvm-config-19 whose version is 19 or later. Without such an
# llvm-config, SRC_askai-code is empty, askai-code is not a target, and the
# rest of the build is unchanged.
ifeq (,$(filter-out default undefined,$(origin LLVM_CONFIG)))
override LLVM_CONFIG := $(shell for c in llvm-config $(foreach v,30 29 28 27 26 25 24 23 22 21 20 19,llvm-config-$v); do \
  v=$$($$c --version 2>/dev/null) && [ "$${v%%.*}" -ge 19 ] && { echo $$c | tee /dev/stderr; break; }; done)
endif

override LLVM_INCLUDEDIR := $(if $(LLVM_CONFIG),$(shell echo $(LLVM_CONFIG) --includedir >&2; $(LLVM_CONFIG) --includedir))

override SRC_askai-code := $(if $(LLVM_CONFIG), \
  buf.c                                         \
  cdb.c                                         \
  child.c                                       \
  confine.c                                     \
  cstr_view.c                                   \
  cxi.c                                         \
  dstr.c                                        \
  jb.c                                          \
  log.c                                         \
  loop.c)

override LIB_askai-code := \
  json-c                   \
  -lclang $(if $(LLVM_CONFIG),-L$(shell echo $(LLVM_CONFIG) --libdir >&2; $(LLVM_CONFIG) --libdir))
