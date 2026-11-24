override SRC_debruijn.so := \
  debruijn_plugin.c         \
  debruijn.c                \
  util.c

# A plugin has no constructor and no writable data of its own (plugin.h).
# The C start files would add both.
override LIB_debruijn.so := \
  json-c                    \
  -nostartfiles             \
  -Wl,-z,relro,-z,now
