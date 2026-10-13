override SRC_test_plugin.so := \
  test_plugin.c

override LIB_test_plugin.so := \
  json-c

# A plugin has no constructor and no writable data of its own (plugin.h).
# The C start files would add both.
override LDFLAGS_test_plugin.so := \
  -nostartfiles                    \
  -Wl,-z,relro,-z,now
