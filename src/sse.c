/** @file sse.c
 *
 * Incremental Server-Sent Events framing.
 */
#include <errno.h>

#include "cstr_view.h"
#include "dstr.h"
#include "sse.h"

/* Call the callback with the event data, without the line feed after the
 * last data line, and then empty the event data. The callback is not
 * called for an event without data lines.
 */
static int
sse_dispatch (struct sse *sse)
{
	size_t len = dstr_len(&sse->data);
	if (!len)
		return 0;

	int e = sse->event(dstr_get(&sse->data), len - 1, sse->user);
	dstr_clear(&sse->data);
	return e;
}

/* Parse @p line, a line of the stream without its line feed. For a data
 * line, append its data and a line feed to the event data. For an empty
 * line, dispatch the event. Ignore other lines.
 */
static int
sse_line (struct sse       *sse,
          struct cstr_view  line)
{
	struct cstr_view const data = cstr_view_sse_data(&line);
	if (!data.data)
		return line.len ? 0 : sse_dispatch(sse);

	return dstr_cat_n(&sse->data, data.data, data.len) &&
	       dstr_cat_n(&sse->data, "\n", 1) ? 0 : ENOMEM;
}

int
sse_feed (struct sse *sse,
          void const *data,
          size_t      len)
{
	if (!dstr_cat_n(&sse->input, data, len))
		return ENOMEM;

	struct cstr_view const input = cstr_view_dstr(&sse->input);
	struct cstr_view rest = input;
	int e = 0;
	for (struct cstr_view line; !e && cstr_view_cut(&rest, '\n', &line);)
		e = sse_line(sse, line);

	dstr_erase(&sse->input, 0, input.len - rest.len);
	return e;
}

void
sse_fini (struct sse *sse)
{
	dstr_cleanup(&sse->data);
	dstr_cleanup(&sse->input);
	sse->event = nullptr;
	sse->user = nullptr;
}
