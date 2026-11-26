#include <errno.h>

#include "cli_client.h"
#include "cli_console.h"
#include "cli_stream.h"
#include "jb.h"

struct cli_stream
cli_stream (struct cli_console *console,
            struct cstr_view    token,
            uint64_t            flags)
{
	return (struct cli_stream){
		.ui = cli_ui(console),
		.text = DSTR_INIT,
		.token = token,
		.flags = { flags },
	};
}

void
cli_stream_fini (struct cli_stream *stream)
{
	cli_ui_fini(&stream->ui);
	dstr_cleanup(&stream->text);
}

/* Show the progress of params, the parameters of a progress notification,
 * if it is for the token of the stream: the turn and the tokens in the
 * status row, and a call of a tool in the line of the calls. The count that
 * grows is the turn while it waits in the server's queue, or else the
 * tokens of the kind named latest, if any.
 */
static int
cli_stream_progress (struct cli_stream  *stream,
                     struct json_object *params)
{
	/* The kinds of the tokens, in the order of their counts. */
	static struct cstr_view const kinds[] = {CSTR_VIEW_INIT("text"), CSTR_VIEW_INIT("reasoning")};
	static_assert(CLI_CONSOLE_COUNT_TEXT + array_size(kinds) == CLI_CONSOLE_COUNT_NONE);
	struct cstr_view token = jb_view(jb_get(params, "progressToken"));
	struct cstr_view message = jb_view(jb_get(params, "message"));
	if (!token.data || !message.data || !cstr_view_equal(token, stream->token))
		return 0;
	struct json_object *meta = jb_get(jb_get(params, "_meta"), "askai");
	struct json_object *tokens = jb_get(meta, "tokens");
	int e = 0;
	/* A server without the member tokens shows no counts. */
	if (tokens) {
		size_t const latest = cstr_view_index(kinds, array_size(kinds), jb_view(jb_get(meta, "latest")));
		e = cli_console_set_progress(stream->ui.console, (struct cli_console_progress){
			.text = jb_uint(jb_get(tokens, "text"), 0),
			.reasoning = jb_uint(jb_get(tokens, "reasoning"), 0),
			.turn = (uint32_t)jb_uint(jb_get(meta, "turn"), 0),
			.active = jb_get(meta, "place") ? CLI_CONSOLE_COUNT_TURN
			                                : (enum cli_console_count)(CLI_CONSOLE_COUNT_TEXT + latest),
		});
	}
	if (e || !jb_is(jb_get(meta, "kind"), "tool"))
		return e;
	/* A server without the member tool names the tool only in the
	 * message. */
	struct cstr_view tool = jb_view(jb_get(meta, "tool"));
	return cli_ui_action(&stream->ui, tool.len ? tool : message);
}

int
cli_stream_notify (void               *context,
                   struct json_object *event,
                   int64_t             request_id,
                   struct cli_error   *error)
{
	struct cli_stream *stream = context;
	struct cstr_view method = jb_view(jb_get(event, "method"));
	struct json_object *params = jb_get(event, "params");
	if (cstr_view_equal(method, CSTR_VIEW("notifications/progress")))
		return cli_stream_progress(stream, params);
	if (!flag_any(stream->flags, CLI_STREAM_DELTAS) ||
	    !cstr_view_equal(method, CSTR_VIEW("notifications/askai/delta")))
		return 0;
	if (jb_int(jb_get(params, "requestId"), -1) != request_id)
		return cli_error_set(error, EPROTO, "Unexpected streaming request id");
	struct cstr_view text = jb_view(jb_get(params, "text"));
	struct cstr_view kind = jb_view(jb_get(params, "kind"));
	if (!text.data || !kind.data)
		return cli_error_set(error, EPROTO, "Expected a JSON string from AskAI");
	int64_t current = jb_int(jb_get(params, "turn"), 0);
	int e = 0;
	if (current != stream->turn) {
		if (dstr_len(&stream->text))
			e = cli_ui_push(&stream->ui, false, CSTR_VIEW("\n"));
		dstr_clear(&stream->text);
		stream->turn = current;
	}
	if (e)
		return e;
	if (cstr_view_equal(kind, CSTR_VIEW("text"))) {
		if (text.len > CLI_LIMIT - dstr_len(&stream->text))
			return cli_error_set(error, EFBIG, "Assistant turn exceeds the client limit");
		if (cstr_view_cat(text, &stream->text))
			return ENOMEM;
		return cli_ui_push(&stream->ui, false, text);
	}
	if (cstr_view_equal(kind, CSTR_VIEW("reasoning")))
		return flag_any(stream->flags, CLI_STREAM_REASONING) ? cli_ui_push(&stream->ui, true, text) : 0;
	return cli_error_set(error, EPROTO, "Unknown AskAI delta kind");
}

/* Show the part of text, the final text of the turn, that the deltas did
 * not show. If the deltas showed the start of text, show the rest of it.
 * If text ends with the deltas, show nothing, because agenda wakeups may
 * include earlier turns that were shown already. Otherwise show all of
 * text on a new line after the deltas, which are not empty then.
 */
static int
cli_stream_rest (struct cli_stream *stream,
                 struct cstr_view   text)
{
	struct cstr_view streamed = cstr_view_dstr(&stream->text);
	if (cstr_view_starts(text, streamed))
		return cli_ui_push(&stream->ui, false, cstr_view_slice(text, streamed.len, SIZE_MAX));
	if (cstr_view_ends(text, streamed))
		return 0;
	int e = cli_ui_push(&stream->ui, false, CSTR_VIEW("\n"));
	return e ? e : cli_ui_push(&stream->ui, false, text);
}

int
cli_stream_result (struct cli_stream   *stream,
                   struct json_object  *result,
                   struct json_object **answer,
                   struct cli_error    *error)
{
	*answer = nullptr;
	struct json_object *first = jb_at(jb_get(result, "content"), 0);
	struct json_object *value = jb_get(first, "text");
	struct cstr_view text = jb_view(value);
	int e = 0;
	if (!first)
		e = cli_error_set(error, EPROTO, "AskAI returned no assistant text");
	else if (!jb_is(jb_get(first, "type"), "text") || !text.data)
		e = cli_error_set(error, EPROTO, "AskAI returned unsupported assistant content");
	else if (json_object_get_boolean(jb_get(result, "isError")))
		e = cli_error_set(error, EIO, "%.*s", (int)text.len, text.data);
	if (!e)
		e = cli_stream_rest(stream, text);
	int finish = cli_ui_finish(&stream->ui);
	if (!e)
		e = finish;
	if (!e)
		*answer = json_object_get(value);
	return e;
}
