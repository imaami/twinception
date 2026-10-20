#include <errno.h>

#include "cli_client.h"
#include "cli_stream.h"
#include "jb.h"

struct cli_stream
cli_stream (struct cli_console *console,
            struct cstr_view    actor,
            struct cstr_view    token,
            uint64_t            flags,
            int                *e)
{
	*e = 0;
	return (struct cli_stream){
		.ui = cli_ui(console),
		.text = DSTR_INIT,
		.actor = actor,
		.token = token,
		.flags = flags,
	};
}

int
cli_stream_init (struct cli_stream  *stream,
                 struct cli_console *console,
                 struct cstr_view    actor,
                 struct cstr_view    token,
                 uint64_t            flags)
{
	int e;
	*stream = cli_stream(console, actor, token, flags, &e);
	return e;
}

void
cli_stream_fini (struct cli_stream *stream)
{
	cli_ui_fini(&stream->ui);
	dstr_cleanup(&stream->text);
}

static int
cli_stream_progress (struct cli_stream  *stream,
                     struct json_object *event)
{
	if (!cli_client_is(jb_get(event, "method"), "notifications/progress"))
		return 0;
	struct json_object *params = jb_get(event, "params");
	struct cstr_view token = cli_client_text(jb_get(params, "progressToken"));
	struct cstr_view message = cli_client_text(jb_get(params, "message"));
	if (!token.data || !message.data || !cstr_view_equal(token, stream->token))
		return 0;
	struct json_object *meta = jb_get(jb_get(params, "_meta"), "askai");
	return cli_client_is(jb_get(meta, "kind"), "tool") ? cli_ui_action(&stream->ui, stream->actor, message)
	                                                   : cli_ui_progress(&stream->ui, stream->actor, message);
}

int
cli_stream_notify (void               *context,
                   struct json_object *event,
                   int64_t             request_id,
                   struct cli_error   *error)
{
	struct cli_stream *stream = context;
	int e = cli_stream_progress(stream, event);
	if (e || !(stream->flags & CLI_STREAM_DELTAS) ||
	    !cli_client_is(jb_get(event, "method"), "notifications/askai/delta"))
		return e;
	struct json_object *params = jb_get(event, "params");
	struct json_object *request = jb_get(params, "requestId");
	if (!json_object_is_type(request, json_type_int) || json_object_get_int64(request) != request_id)
		return cli_error_set(error, EPROTO, "Unexpected streaming request id");
	struct cstr_view text = cli_client_text(jb_get(params, "text"));
	struct cstr_view kind = cli_client_text(jb_get(params, "kind"));
	if (!text.data || !kind.data)
		return cli_error_set(error, EPROTO, "Expected a JSON string from AskAI");
	int64_t current = json_object_get_int64(jb_get(params, "turn"));
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
		if (!dstr_cat_n(&stream->text, text.data, text.len))
			return ENOMEM;
		return cli_ui_push(&stream->ui, false, text);
	}
	if (cstr_view_equal(kind, CSTR_VIEW("reasoning")))
		return stream->flags & CLI_STREAM_REASONING ? cli_ui_push(&stream->ui, true, text) : 0;
	return cli_error_set(error, EPROTO, "Unknown AskAI delta kind");
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
	struct cstr_view text = cli_client_text(value);
	int e = 0;
	if (!first)
		e = cli_error_set(error, EPROTO, "AskAI returned no assistant text");
	else if (!cli_client_is(jb_get(first, "type"), "text") || !text.data)
		e = cli_error_set(error, EPROTO, "AskAI returned unsupported assistant content");
	else if (json_object_get_boolean(jb_get(result, "isError")))
		e = cli_error_set(error, EIO, "%.*s", (int)text.len, text.data);
	if (!e) {
		struct cstr_view streamed = cstr_view_dstr(&stream->text);
		if (cstr_view_starts(text, streamed))
			e = cli_ui_push(&stream->ui, false, cstr_view_slice(text, streamed.len, SIZE_MAX));
		else if (streamed.len && text.len >= streamed.len &&
		         cstr_view_equal(cstr_view_slice(text, text.len - streamed.len, SIZE_MAX), streamed)) {
			/* Agenda wakeups may include already displayed earlier turns. */
		} else {
			if (streamed.len)
				e = cli_ui_push(&stream->ui, false, CSTR_VIEW("\n"));
			if (!e)
				e = cli_ui_push(&stream->ui, false, text);
		}
	}
	int finish = cli_ui_finish(&stream->ui);
	if (!e)
		e = finish;
	if (!e)
		*answer = json_object_get(value);
	return e;
}

int
cli_stream_action (struct cli_stream *stream,
                   struct cstr_view   actor,
                   struct cstr_view   text)
{
	return cli_ui_action(&stream->ui, actor, text);
}
