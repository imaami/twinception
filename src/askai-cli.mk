override SRC_askai-cli := \
  autocomplete.c          \
  autocomplete_file.c     \
  blocker.c               \
  buf.c                   \
  cli.c                   \
  cli_client.c            \
  cli_command.c           \
  cli_console.c           \
  cli_io.c                \
  cli_loop.c              \
  cli_mailbox.c           \
  cli_network.c           \
  cli_render.c            \
  cli_scrollback.c        \
  cli_signals.c           \
  cli_state.c             \
  cli_stream.c            \
  cli_style.c             \
  cli_tool.c              \
  cli_ui.c                \
  cstr_view.c             \
  dstr.c                  \
  jb.c                    \
  log.c                   \
  loop.c

override LIB_askai-cli := \
  json-c                  \
  libcurl                 \
  readline

override LDLIBS_askai-cli += -pthread -lm
