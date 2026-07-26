/** @file Incremental Server-Sent Events framing. */
#include <errno.h>
#include <string.h>

#include "sse.h"

static int
sse_dispatch (struct sse *sse)
{
	if (!sse->data.len)
		return 0;

	if (sse->data.data[sse->data.len - 1] == '\n') {
		--sse->data.len;
		sse->data.data[sse->data.len] = '\0';
	}

	int e = sse->event(sse->data.data, sse->data.len, sse->user);
	buf_reset(&sse->data);
	return e;
}

static int
sse_line (struct sse *sse,
          char       *line,
          size_t      len)
{
	if (len && line[len - 1] == '\r')
		--len;

	if (!len)
		return sse_dispatch(sse);

	if (len < 5 || memcmp(line, "data:", 5))
		return 0;

	line += 5;
	len -= 5;
	if (len && *line == ' ') {
		++line;
		--len;
	}

	int e = buf_append(&sse->data, line, len);
	return e ? e : buf_append_char(&sse->data, '\n');
}

void
sse_init (struct sse *sse,
          sse_event_cb *event,
          void         *user)
{
	*sse = (struct sse){
		.event = event,
		.user = user
	};
}

int
sse_feed (struct sse *sse,
          void const *data,
          size_t      len)
{
	int e = buf_append(&sse->input, data, len);
	if (e)
		return e;

	size_t used = 0;
	while (used < sse->input.len) {
		char *line = sse->input.data + used;
		char *nl = memchr(line, '\n', sse->input.len - used);
		if (!nl)
			break;

		size_t line_len = (size_t)(nl - line);
		e = sse_line(sse, line, line_len);
		used += line_len + 1;
		if (e)
			break;
	}

	if (used)
		buf_consume(&sse->input, used);
	return e;
}

int
sse_finish (struct sse *sse)
{
	int e = 0;
	if (sse->input.len)
		e = sse_line(sse, sse->input.data, sse->input.len);
	buf_reset(&sse->input);
	return e ? e : sse_dispatch(sse);
}

void
sse_fini (struct sse *sse)
{
	buf_fini(&sse->data);
	buf_fini(&sse->input);
	*sse = (struct sse){0};
}
