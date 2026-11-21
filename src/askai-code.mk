# askai-code, the helper program of the code tool, links libclang 19 or
# later. The build uses the llvm-config in LLVM_CONFIG if it is set on the
# command line or in the environment, or else the first of llvm-config and
# llvm-config-30 to llvm-config-19 whose version is 19 or later. Without such an
# llvm-config, SRC_askai-code is empty, askai-code is not a target, and the
# rest of the build is unchanged. cxi_ast.cpp is C++ and needs the C++ headers
# of clang and LLVM of the same llvm-config, and the LLVM library.
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
  cxi_ast.cpp                                   \
  cxi_out.c                                     \
  dstr.c                                        \
  fs.c                                          \
  graph.c                                       \
  jb.c                                          \
  log.c                                         \
  loop.c                                        \
  tty.c                                         \
  util.c)

override LIB_askai-code := \
  json-c                   \
  libcrypto                \
  -lclang $(if $(LLVM_CONFIG),-L$(shell echo $(LLVM_CONFIG) --libdir >&2; $(LLVM_CONFIG) --libdir) \
                              $(shell echo $(LLVM_CONFIG) --libs support >&2; $(LLVM_CONFIG) --libs support))

# The C++ options of the LLVM headers, with the include directory as a system
# directory, as for cxi.c. Without LTO the object also links if CC and CXX
# are of different makers, such as gcc and clang++.
override CPPFLAGS_cxi_ast.cpp = -isystem $(LLVM_INCLUDEDIR) $(LLVM_CXXFLAGS)
override LLVM_CXXFLAGS := $(if $(LLVM_CONFIG),$(filter-out -I%,$(shell echo $(LLVM_CONFIG) --cxxflags >&2; $(LLVM_CONFIG) --cxxflags)))
%cxi_ast.cpp.o: override CXXFLAGS += -fno-lto -fno-rtti
