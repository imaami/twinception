/** @file Transcript bounds, wrapping and viewport stability. */
#include <assert.h>
#include <errno.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cli_scrollback.h"

static void
cli_scrollback_test_render (struct cli_scrollback *scrollback,
                            size_t                 columns,
                            size_t                 height,
                            struct dstr           *frame)
{
	dstr_clear(frame);
	assert(!cli_scrollback_render(scrollback, columns, height, frame));
}

static void
cli_scrollback_test_wrapping (void)
{
	struct cli_scrollback scrollback;
	cli_scrollback_init(&scrollback);
	dstr_auto frame = dstr(nullptr);
	assert(!cli_scrollback_append(&scrollback, CSTR_VIEW("界界界\néx\n")));
	cli_scrollback_test_render(&scrollback, 4, 4, &frame);
	assert(!strcmp(dstr_get(&frame), "界界\r\n界\033[K\r\néx\033[K\r\n\033[K\r\n"));
	cli_scrollback_test_render(&scrollback, 8, 3, &frame);
	assert(!strcmp(dstr_get(&frame), "界界界\033[K\r\néx\033[K\r\n\033[K\r\n"));
	cli_scrollback_fini(&scrollback);
	assert(!cli_scrollback_append(&scrollback, CSTR_VIEW("abcd\tX")));
	cli_scrollback_test_render(&scrollback, 5, 3, &frame);
	assert(!strcmp(dstr_get(&frame), "abcd\033[K\r\n     \r\nX\033[K\r\n"));
	cli_scrollback_fini(&scrollback);
	assert(!cli_scrollback_append(&scrollback, CSTR_VIEW("abcdeFGHIJk")));
	cli_scrollback_test_render(&scrollback, 5, 3, &frame);
	assert(!strcmp(dstr_get(&frame), "abcde\r\nFGHIJ\r\nk\033[K\r\n"));
	cli_scrollback_test_render(&scrollback, 11, 1, &frame);
	assert(!strcmp(dstr_get(&frame), "abcdeFGHIJk\r\n"));
	cli_scrollback_fini(&scrollback);
}

static void
cli_scrollback_test_viewport (void)
{
	struct cli_scrollback scrollback;
	cli_scrollback_init(&scrollback);
	for (size_t i = 0; i < 10000; ++i) {
		dstr_auto line = dstr(nullptr);
		assert(dstr_printf(&line, "line %zu\n", i));
		assert(!cli_scrollback_append(&scrollback, cstr_view_dstr(&line)));
	}
	assert(scrollback.count == 10000);
	dstr_auto before = dstr(nullptr);
	dstr_auto after = dstr(nullptr);
	cli_scrollback_test_render(&scrollback, 40, 4, &before);
	cli_scrollback_scroll(&scrollback, 20, true);
	cli_scrollback_test_render(&scrollback, 40, 4, &before);
	for (size_t i = 0; i < 10; ++i)
		assert(!cli_scrollback_append(&scrollback, CSTR_VIEW("new line\n")));
	cli_scrollback_test_render(&scrollback, 40, 4, &after);
	assert(cstr_view_equal(cstr_view_dstr(&before), cstr_view_dstr(&after)));
	assert(scrollback.unread);
	assert(scrollback.count == 10000);
	cli_scrollback_bottom(&scrollback);
	cli_scrollback_test_render(&scrollback, 40, 4, &after);
	assert(!scrollback.offset && !scrollback.unread);
	assert(strstr(dstr_get(&after), "new line"));
	cli_scrollback_fini(&scrollback);
}

static void
cli_scrollback_test_styles (void)
{
	struct cli_scrollback scrollback;
	cli_scrollback_init(&scrollback);
	dstr_auto frame = dstr(nullptr);
	assert(!cli_style_sequence(CLI_STYLE_COUNT).len);
	assert(cli_scrollback_append_style(&scrollback, CSTR_VIEW("invalid"), CLI_STYLE_COUNT) == EINVAL);
	assert(!scrollback.count && !scrollback.bytes && !scrollback.styles);
	assert(!cli_scrollback_append_style(&scrollback, CSTR_VIEW("abc"), CLI_STYLE_REASONING));
	assert(!cli_scrollback_append_style(&scrollback, CSTR_VIEW("def"), CLI_STYLE_REASONING));
	assert(scrollback.styles == 1);
	assert(!cli_scrollback_append_style(&scrollback, CSTR_VIEW("界\tx"), CLI_STYLE_USER_INPUT));
	assert(!cli_scrollback_append(&scrollback, CSTR_VIEW("!")));
	cli_scrollback_test_render(&scrollback, 8, 3, &frame);
	assert(!strcmp(dstr_get(&frame),
	               "\033[0;90mabcdef\033[0;1;32m界\r\n"
	               "        \r\nx\033[0m!\033[K\r\n"));
	cli_scrollback_test_render(&scrollback, 3, 1, &frame);
	assert(!strcmp(dstr_get(&frame), "\033[0;1;32mx\033[0m!\033[K\r\n"));
	cli_scrollback_scroll(&scrollback, 2, true);
	cli_scrollback_test_render(&scrollback, 3, 1, &frame);
	assert(!strcmp(dstr_get(&frame), "\033[0;1;32m界\033[K\r\n\033[0m"));
	cli_scrollback_fini(&scrollback);

	for (size_t i = 0; i < 65536; ++i)
		assert(!cli_scrollback_append_style(&scrollback, CSTR_VIEW("x"), i % 2 ? CLI_STYLE_REASONING : CLI_STYLE_USER_INPUT));
	assert(scrollback.styles == 65536);
	assert(!cli_scrollback_append_style(&scrollback, CSTR_VIEW("x\n"), CLI_STYLE_REASONING));
	assert(scrollback.bytes == 65537 && scrollback.count == 2);
	assert(scrollback.styles == 65536);
	cli_scrollback_fini(&scrollback);

	/* Style storage has its own bound, even with one change per byte. */
	for (size_t i = 0; i < 70000; ++i)
		assert(!cli_scrollback_append_style(&scrollback, CSTR_VIEW("x"), i % 2 ? CLI_STYLE_REASONING : CLI_STYLE_USER_INPUT));
	assert(scrollback.styles <= 65536);
	assert(scrollback.bytes < 70000);
	cli_scrollback_test_render(&scrollback, 2, 1, &frame);
	assert(!strcmp(dstr_get(&frame), "\033[0;1;32mx\033[0;90mx\r\n\033[0m"));
	cli_scrollback_fini(&scrollback);
}

static void
cli_scrollback_test_prefix_eviction (void)
{
	struct cli_scrollback scrollback;
	cli_scrollback_init(&scrollback);
	size_t length = 16 * 1024 * 1024;
	char *text = malloc(length);
	assert(text);
	for (size_t i = 0; i < length; ++i)
		text[i] = (char)('a' + i / 80 % 26);
	assert(!cli_scrollback_append_style(&scrollback, (struct cstr_view){text, length}, CLI_STYLE_INFO));
	dstr_auto before = dstr(nullptr);
	dstr_auto after = dstr(nullptr);
	cli_scrollback_test_render(&scrollback, 80, 3, &before);
	cli_scrollback_scroll(&scrollback, 10, true);
	cli_scrollback_test_render(&scrollback, 80, 3, &before);
	memset(text, 'Z', 80);
	assert(!cli_scrollback_append(&scrollback, (struct cstr_view){text, 80}));
	cli_scrollback_test_render(&scrollback, 80, 3, &after);
	assert(cstr_view_equal(cstr_view_dstr(&before), cstr_view_dstr(&after)));
	assert(scrollback.offset == 11);
	/* A partial-row eviction must not rewrap the retained viewport. */
	assert(!cli_scrollback_append(&scrollback, CSTR_VIEW("Z")));
	cli_scrollback_test_render(&scrollback, 80, 3, &after);
	assert(cstr_view_equal(cstr_view_dstr(&before), cstr_view_dstr(&after)));
	assert(scrollback.bytes <= length);
	free(text);
	text = nullptr;
	cli_scrollback_fini(&scrollback);
}

static void
cli_scrollback_test_long_line (void)
{
	struct cli_scrollback scrollback;
	cli_scrollback_init(&scrollback);
	size_t length = 17 * 1024 * 1024;
	char *text = malloc(length);
	assert(text);
	memset(text, 'a', length);
	assert(!cli_scrollback_append(&scrollback, (struct cstr_view){text, length}));
	assert(scrollback.bytes == 16 * 1024 * 1024);
	assert(scrollback.count == 1);
	dstr_auto frame = dstr(nullptr);
	cli_scrollback_test_render(&scrollback, 80, 3, &frame);
	assert(dstr_len(&frame) < 300);
	cli_scrollback_fini(&scrollback);
	/* Repeated streaming redraws reuse the unchanged prefix's wrapping.
	 * Checkpoints also bound the work needed to reach the visible tail. */
	assert(!cli_scrollback_append(&scrollback, (struct cstr_view){text, 2 * 1024 * 1024}));
	for (size_t i = 0; i < 1024; ++i) {
		assert(!cli_scrollback_append(&scrollback, CSTR_VIEW("z")));
		cli_scrollback_test_render(&scrollback, 80, 3, &frame);
		assert(strchr(dstr_get(&frame), 'z'));
		assert(dstr_len(&frame) < 300);
	}
	/* A new width invalidates checkpoints without changing the transcript. */
	cli_scrollback_test_render(&scrollback, 17, 3, &frame);
	assert(strchr(dstr_get(&frame), 'z'));
	assert(dstr_len(&frame) < 100);
	free(text);
	text = nullptr;
	cli_scrollback_fini(&scrollback);
}

int
main (void)
{
	assert(setlocale(LC_CTYPE, "C.UTF-8"));
	cli_scrollback_test_wrapping();
	cli_scrollback_test_styles();
	cli_scrollback_test_viewport();
	cli_scrollback_test_long_line();
	cli_scrollback_test_prefix_eviction();
	puts("cli scrollback: UTF-8 wrapping, viewport, bounds and streaming redraw: PASS");
	return 0;
}
