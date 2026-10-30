/** @file cli_client.h
 *
 * Unix-socket MCP connection, independent of terminal and history.
 */
#ifndef TWINCEPTION_SRC_CLI_CLIENT_H_
#define TWINCEPTION_SRC_CLI_CLIENT_H_

#include <json-c/json.h>
#include <stdint.h>

#include "autocomplete.h"
#include "cstr_view.h"
#include "dstr.h"

enum { CLI_LIMIT = 16 * 1024 * 1024 };

/** Optional detail accompanying an error number; no allocation on failure. */
struct cli_error {
	char message[2048];
};

__attribute__((format(printf, 3, 4)))
extern int
cli_error_set (struct cli_error *error,
               int               e,
               char const       *format, ...);

extern struct cstr_view
cli_error_text (struct cli_error const *error,
                int                     e);

struct cli_network;
struct cli_client;

/** Optional UI-thread handler for nonfatal discovery/cancellation details. */
typedef int cli_client_report_fn (struct cli_client *client,
                                  struct cstr_view    title,
                                  struct cstr_view    detail);

/** Strings and the catalog are owned; network is borrowed. The client and
 * its protocol callbacks are confined to the UI thread.
 */
struct cli_client {
	struct dstr            socket;
	struct dstr            session;
	struct dstr            tool;
	struct dstr            session_key;
	struct dstr            participant;
	struct dstr            notes_tool;
	struct autocomplete   *catalog;
	struct json_object    *notebooks;
	struct cli_network    *network;
	cli_client_report_fn  *report;
	int64_t                next_id;
	bool                   catalog_dirty;
	bool                   notes_dirty;
	bool                   identity_changed;
};

extern struct cli_client
cli_client (char const            *socket,
            char const            *tool,
            struct cli_network    *network,
            int                   *e);

extern void
cli_client_fini (struct cli_client *client);

extern int
cli_client_connect (struct cli_client *client,
                    struct cli_error  *error);

extern int
cli_client_refresh (struct cli_client *client,
                    struct cli_error  *error);

extern int
cli_client_refresh_notes (struct cli_client *client,
                          struct cli_error  *error);

typedef int
cli_client_notify_fn (void               *context,
                      struct json_object *event,
                      int64_t             request_id,
                      struct cli_error   *error);

/** Consumes params on every path. On success the caller owns *result;
 * on failure *result is null. Notifications borrow their event.
 */
extern int
cli_client_rpc (struct cli_client     *client,
                char const            *method,
                struct json_object    *params,
                cli_client_notify_fn  *notify,
                void                  *context,
                struct json_object   **result,
                struct cli_error      *error);

/** Runs on the UI thread; result and error are borrowed for the callback. */
typedef void cli_client_done_fn (void                   *context,
                                 int                     e,
                                 struct json_object     *result,
                                 struct cli_error const *error);

/** Consumes params. Success schedules exactly one completion; failure does
 * not call done. The returned network request ID supports cancellation.
 */
extern int
cli_client_submit (struct cli_client    *client,
                   char const           *method,
                   struct json_object   *params,
                   cli_client_notify_fn *notify,
                   cli_client_done_fn   *done,
                   void                 *context,
                   uint64_t             *request,
                   struct cli_error     *error);

/** Cancel a request admitted by submit, without affecting other calls. */
extern void
cli_client_cancel_request (struct cli_client *client,
                           uint64_t           request);

/**
 * Invalid/non-string values yield an
 * empty view with a null data pointer.
 */
extern struct cstr_view
cli_client_text (struct json_object *value);

/** Protocol strings are counted: an embedded NUL must not hide a suffix. */
static inline bool
cli_client_is (struct json_object *value,
               char const         *text)
{
	return cstr_view_equal(cli_client_text(value), cstr_view(text));
}

extern struct json_object *
cli_client_string (struct cstr_view text);

extern int
cli_client_wire (struct json_object *value,
                 int                 flags,
                 struct cstr_view   *wire,
                 struct cli_error   *error);

static inline void
cli_client_json_cleanup (struct json_object **value)
{
	json_object_put(*value);
	*value = nullptr;
}

#define cli_json_auto __attribute__((cleanup(cli_client_json_cleanup))) struct json_object *

#endif /* TWINCEPTION_SRC_CLI_CLIENT_H_ */
