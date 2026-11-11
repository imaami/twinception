override SRC_godbolt.so := \
  godbolt.c                \
  godbolt_probe.c

# A plugin has no constructor and no writable data of its own (plugin.h).
# The C start files would add both.
override LIB_godbolt.so := \
  json-c                   \
  -nostartfiles            \
  -Wl,-z,relro,-z,now
