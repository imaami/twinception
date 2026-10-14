override SRC_godbolt.so := \
  godbolt.c

override LIB_godbolt.so := \
  json-c

# A plugin has no constructor and no writable data of its own (plugin.h).
# The C start files would add both.
override LDFLAGS_godbolt.so := \
  -nostartfiles               \
  -Wl,-z,relro,-z,now
