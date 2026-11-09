override SRC_askai := \
  agenda.c            \
  ai.c                \
  askai.c             \
  blocker.c           \
  brainstem.c         \
  buf.c               \
  child.c             \
  code.c              \
  config.c            \
  cstr_view.c         \
  curl_loop.c         \
  datauri.c           \
  dstr.c              \
  evidence.c          \
  finding.c           \
  findings.c          \
  git.c               \
  git_run.c           \
  git_view.c          \
  home.c              \
  http.c              \
  httpd.c             \
  instance.c          \
  jb.c                \
  journal.c           \
  ledger.c            \
  log.c               \
  loop.c              \
  manual.c            \
  mcp.c               \
  member.c            \
  metrics.c           \
  notes.c             \
  notes_tool.c        \
  notice.c            \
  plugin.c            \
  policy.c            \
  registry.c          \
  roster.c            \
  schema.c            \
  seen.c              \
  store.c             \
  task.c              \
  tls.c               \
  uploads.c           \
  web.c

override LIB_askai := \
  libcrypto           \
  json-c              \
  libcurl             \
  libssl

# Exports to the plugins it loads what src/export.h marks
override LDFLAGS_askai := -rdynamic
