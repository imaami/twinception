override SRC_askai-cli := \
  autocomplete.c          \
  autocomplete_file.c     \
  buf.c                   \
  cli.c                   \
  cli_client.c            \
  cli_console.c           \
  cli_state.c             \
  cli_ui.c                \
  cstr_view.c             \
  dstr.c                  \
  jb.c

override LIB_askai-cli := \
  json-c                  \
  libcurl                 \
  readline

override LDLIBS_askai-cli += -lm
