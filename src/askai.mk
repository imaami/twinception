override SRC_askai := \
  ai.c                \
  askai.c             \
  blocker.c           \
  brainstem.c         \
  buf.c               \
  child.c             \
  config.c            \
  dstr.c              \
  git.c               \
  home.c              \
  http.c              \
  httpd.c             \
  instance.c          \
  jb.c                \
  ledger.c            \
  log.c               \
  loop.c              \
  manual.c            \
  mcp.c               \
  metrics.c           \
  notes.c             \
  plugin.c            \
  policy.c            \
  registry.c          \
  roster.c            \
  task.c              \
  tls.c

override LIB_askai := \
  libcrypto           \
  json-c              \
  libcurl             \
  libssl

# Exports to the plugins it loads what src/export.h marks
override LDFLAGS_askai := -rdynamic
