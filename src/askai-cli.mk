override SRC_askai-cli := \
  autocomplete.c          \
  autocomplete-file.c     \
  buf.c                   \
  cli.c                   \
  cli-client.c            \
  cli-console.c           \
  cli-state.c             \
  cli-ui.c                \
  dstr.c                  \
  jb.c

override LIB_askai-cli := \
  json-c                  \
  libcurl                 \
  readline

override LDLIBS_askai-cli += -lm
