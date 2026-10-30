/** @file sse.c
 *
 * Incremental Server-Sent Events framing.
 */
#include <errno.h>
#include <string.h>

#include "dstr.h"
#include "sse.h"

static int
sse_dispatch (struct sse *sse)
{
	size_t len = dstr_len(&sse->data);
	if (!len)
		return 0;

	if (dstr_get(&sse->data)[len - 1] == '\n')
		dstr_erase(&sse->data, --len, 1);

	int e = sse->event(dstr_get(&sse->data), len, sse->user);
	dstr_clear(&sse->data);
	return e;
}

static int
sse_line (struct sse *sse,
          char const *line,
          size_t      len)
{
	if (len && line[len - 1] == '\r')
		--len;

	if (!len)
		return sse_dispatch(sse);

	static char const field[] = "data:";
	size_t n = sizeof field - 1;
	if (len < n || memcmp(line, field, n))
		return 0;

	line += n;
	len -= n;
	if (len && *line == ' ') {
		++line;
		--len;
	}

	return dstr_cat_n(&sse->data, line, len) &&
	       dstr_cat_n(&sse->data, "\n", 1) ? 0 : ENOMEM;
}

struct sse
sse (sse_event_cb *event,
     void         *user)
{
	return (struct sse){
		.input = DSTR_INIT,
		.data = DSTR_INIT,
		.event = event,
		.user = user
	};
}

void
sse_reset (struct sse *sse)
{
	dstr_clear(&sse->input);
	dstr_clear(&sse->data);
}

int
sse_feed (struct sse *sse,
          void const *data,
          size_t      len)
{
	if (!dstr_cat_n(&sse->input, data, len))
		return ENOMEM;

	char const *input = dstr_get(&sse->input);
	size_t input_len = dstr_len(&sse->input);
	size_t used = 0;
	int e = 0;
	while (used < input_len) {
		char const *line = input + used;
		char const *nl = memchr(line, '\n', input_len - used);
		if (!nl)
			break;

		size_t line_len = (size_t)(nl - line);
		e = sse_line(sse, line, line_len);
		used += line_len + 1;
		if (e)
			break;
	}

	dstr_erase(&sse->input, 0, used);
	return e;
}

int
sse_end (struct sse *sse)
{
	size_t len = dstr_len(&sse->input);
	int e = len ? sse_line(sse, dstr_get(&sse->input), len) : 0;
	dstr_clear(&sse->input);
	return e ? e : sse_dispatch(sse);
}

void
sse_fini (struct sse *sse)
{
	dstr_cleanup(&sse->data);
	dstr_cleanup(&sse->input);
	sse->event = nullptr;
	sse->user = nullptr;
}
