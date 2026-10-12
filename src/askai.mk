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
  jb.c                \
  log.c               \
  loop.c              \
  manual.c            \
  mcp.c               \
  metrics.c           \
  policy.c            \
  task.c              \
  tls.c

override LIB_askai := \
  libcrypto           \
  json-c              \
  libcurl             \
  libssl
