/** @file Per-request progress, deltas and assistant result rendering. */
#ifndef TWINCEPTION_SRC_CLI_STREAM_H_
#define TWINCEPTION_SRC_CLI_STREAM_H_

#include <stdint.h>

#include "cli_ui.h"
#include "cstr_view.h"
#include "dstr.h"

struct cli_console;
struct cli_error;
struct json_object;

enum {
	CLI_STREAM_DELTAS = 1u << 0,
	CLI_STREAM_REASONING = 1u << 1,
};

/** Owns rendering state and accumulated text; borrows its console, actor
 * and token. The borrowed strings must remain readable and unchanged until
 * fini. Members provide storage for embedding, not a caller interface. */
struct cli_stream {
	struct cli_ui    ui;
	struct dstr      text;
	struct cstr_view actor;
	struct cstr_view token;
	int64_t          turn;
	uint64_t         flags;
};

extern struct cli_stream
cli_stream (struct cli_console *console,
            struct cstr_view    actor,
            struct cstr_view    token,
            uint64_t            flags,
            int                *e);

extern int
cli_stream_init (struct cli_stream  *stream,
                 struct cli_console *console,
                 struct cstr_view    actor,
                 struct cstr_view    token,
                 uint64_t            flags);

/** Finishes pending output and releases owned storage. */
extern void
cli_stream_fini (struct cli_stream *stream);

/** cli_client notification callback. Always handles matching progress;
 * assistant deltas require CLI_STREAM_DELTAS. */
extern int
cli_stream_notify (void               *context,
                   struct json_object *event,
                   int64_t             request_id,
                   struct cli_error   *error);

/** Reconciles final assistant text with streamed deltas and finishes output.
 * Borrows result; gives the caller an owned answer, or null on failure. */
extern int
cli_stream_result (struct cli_stream   *stream,
                   struct json_object  *result,
                   struct json_object **answer,
                   struct cli_error    *error);

extern int
cli_stream_action (struct cli_stream *stream,
                   struct cstr_view   actor,
                   struct cstr_view   text);

#endif /* TWINCEPTION_SRC_CLI_STREAM_H_ */
