/** @file Unix-socket MCP connection, independent of terminal and history. */
#ifndef TWINCEPTION_SRC_CLI_CLIENT_H_
#define TWINCEPTION_SRC_CLI_CLIENT_H_

#include <json-c/json.h>
#include <signal.h>
#include <stdint.h>

#include "autocomplete.h"
#include "cli-text.h"

enum { CLI_LIMIT = 16 * 1024 * 1024 };
#define CLI_VERSION "2025-11-25"

/** Optional detail accompanying an error number; no allocation on failure. */
struct cli_error {
	char message[2048];
};
__attribute__((format(printf, 3, 4)))
int cli_error_set(struct cli_error *error, int e, char const *format, ...);
struct cli_text cli_error_text(struct cli_error const *error, int e);

/** All strings and the catalog are owned. Signal state and wake_fd are
 * borrowed. A client is confined to its calling thread. */
struct cli_client {
	struct dstr socket, session, tool, session_key, participant, notes_tool;
	struct autocomplete *catalog;
	struct json_object *notebooks;
	volatile sig_atomic_t *interrupted;
	int64_t next_id;
	int wake_fd;
	bool catalog_dirty, notes_dirty, identity_changed;
};
struct cli_client cli_client(char const *socket, char const *tool,
                             volatile sig_atomic_t *interrupted, int wake_fd, int *e);
void cli_client_fini(struct cli_client *client);
int cli_client_connect(struct cli_client *client, struct cli_error *error);
int cli_client_refresh(struct cli_client *client, struct cli_error *error);
int cli_client_refresh_notes(struct cli_client *client, struct cli_error *error);

typedef int cli_client_notify_fn(void *context, struct json_object *event,
                                 int64_t request_id, struct cli_error *error);
/** Consumes params on every path. On success the caller owns *result;
 * on failure *result is null. Notifications borrow their event. */
int cli_client_rpc(struct cli_client *client, char const *method,
                    struct json_object *params, cli_client_notify_fn *notify,
                    void *context, struct json_object **result, struct cli_error *error);

/** Invalid/non-string values yield an empty view with a null data pointer. */
struct cli_text cli_client_text(struct json_object *value);
/** Protocol strings are counted: an embedded NUL must not hide a suffix. */
static inline bool cli_client_is(struct json_object *value, char const *text)
{
	return cli_text_equal(cli_client_text(value), cli_text_cstr(text));
}
struct json_object *cli_client_string(struct cli_text text);
int cli_client_wire(struct json_object *value, int flags,
                     struct cli_text *wire, struct cli_error *error);

static inline void cli_client_json_cleanup(struct json_object **value)
{
	json_object_put(*value);
	*value = nullptr;
}
#define cli_json_auto __attribute__((cleanup(cli_client_json_cleanup))) struct json_object *

#endif /* TWINCEPTION_SRC_CLI_CLIENT_H_ */
