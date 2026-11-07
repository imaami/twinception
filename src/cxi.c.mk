override INC_cxi.c := json-c
# The libclang headers, from the llvm-config of src/askai-code.mk. With
# -isystem, they are system headers, so warnings such as -Wpadded are not
# reported for them.
override CPPFLAGS_cxi.c = -isystem $(LLVM_INCLUDEDIR)
