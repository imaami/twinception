/** @file Transcript bounds, wrapping and viewport stability. */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cli_render.h"
#include "cli_scrollback.h"

static void
cli_scrollback_test_render (struct cli_scrollback *scrollback,
                            size_t                 columns,
                            size_t                 height,
                            bool                   color,
                            struct dstr           *frame)
{
	dstr_clear(frame);
	assert(!cli_scrollback_render_pane(scrollback, 1, 1, columns, height, color, nullptr, frame));
}

static void
cli_scrollback_test_parts (struct dstr const      *frame,
                           struct cstr_view const *parts,
                           size_t                  count)
{
	struct cstr_view text = cstr_view_dstr(frame);
	for (size_t i = 0; i < count; ++i) {
		assert(cstr_view_starts(text, parts[i]));
		text = cstr_view_slice(text, parts[i].len, SIZE_MAX);
	}
	assert(!text.len);
}

#define CLI_SCROLLBACK_FRAME(frame, ...) do { \
	struct cstr_view const parts[] = {__VA_ARGS__}; \
	cli_scrollback_test_parts(frame, parts, sizeof parts / sizeof *parts); \
} while (0)

static void
cli_scrollback_test_characters (void)
{
	struct {
		struct cstr_view text;
		uint32_t        bytes;
		int             width;
	} const cases[] = {
		{CSTR_VIEW("a"), 1, 1},
		{CSTR_VIEW("éx"), 2, 1},
		{CSTR_VIEW("界x"), 3, 2},
		{CSTR_VIEW("\U0001fae8"), 4, 2},
		{CSTR_VIEW("\u0301x"), 2, 0},
		{CSTR_VIEW("\u20dd"), 3, 0},
		{CSTR_VIEW("\u200d"), 3, 0},
		{CSTR_VIEW("\ufe0f"), 3, 0},
		{CSTR_VIEW("\u00b7"), 2, 1},
		{CSTR_VIEW("\u0378"), 2, 1},
		{CSTR_VIEW("\ue000"), 3, 1},
		{CSTR_VIEW("\U0010ffff"), 4, 1},
		{CSTR_VIEW("\0"), 1, -1},
		{CSTR_VIEW("\033"), 1, -1},
		{CSTR_VIEW("\177"), 1, -1},
		{CSTR_VIEW("\xc2\x9b"), 2, -1},
		{CSTR_VIEW("\x80"), 1, -1},
		{CSTR_VIEW("\xc0\xaf"), 1, -1},
		{CSTR_VIEW("\xc2"), 1, -1},
		{CSTR_VIEW("\xc2x"), 1, -1},
		{CSTR_VIEW("\xe0\x80\x80"), 1, -1},
		{CSTR_VIEW("\xed\xa0\x80"), 1, -1},
		{CSTR_VIEW("\xe7\x95"), 1, -1},
		{CSTR_VIEW("\xf0\x80\x80\x80"), 1, -1},
		{CSTR_VIEW("\xf4\x90\x80\x80"), 1, -1},
		{CSTR_VIEW("\xf0\x9f\x98"), 1, -1},
		{CSTR_VIEW("\xff"), 1, -1},
	};
	for (size_t i = 0; i < sizeof cases / sizeof *cases; ++i) {
		int width;
		assert(cli_render_character(cases[i].text, &width) == cases[i].bytes);
		assert(width == cases[i].width);
	}
}

static void
cli_scrollback_test_wrapping (void)
{
	struct cli_scrollback scrollback;
	cli_scrollback_init(&scrollback);
	dstr_auto frame = dstr(nullptr);
	assert(!cli_scrollback_append(&scrollback, CSTR_VIEW("界界界\néx\n")));
	cli_scrollback_test_render(&scrollback, 4, 4, false, &frame);
	assert(!strcmp(dstr_get(&frame), "\033[1;1H界界\033[2;1H界  \033[3;1Héx  \033[4;1H    "));
	cli_scrollback_test_render(&scrollback, 8, 3, false, &frame);
	assert(!strcmp(dstr_get(&frame), "\033[1;1H界界界  \033[2;1Héx      \033[3;1H        "));
	cli_scrollback_fini(&scrollback);
	assert(!cli_scrollback_append(&scrollback, CSTR_VIEW("abcd\tX")));
	cli_scrollback_test_render(&scrollback, 5, 3, false, &frame);
	assert(!strcmp(dstr_get(&frame), "\033[1;1Habcd \033[2;1H     \033[3;1HX    "));
	cli_scrollback_fini(&scrollback);
	assert(!cli_scrollback_append(&scrollback, CSTR_VIEW("abcdeFGHIJk")));
	cli_scrollback_test_render(&scrollback, 5, 3, false, &frame);
	assert(!strcmp(dstr_get(&frame), "\033[1;1Habcde\033[2;1HFGHIJ\033[3;1Hk    "));
	cli_scrollback_test_render(&scrollback, 11, 1, false, &frame);
	assert(!strcmp(dstr_get(&frame), "\033[1;1HabcdeFGHIJk"));
	cli_scrollback_fini(&scrollback);
	/* U+0378 is unassigned: an unknown width must not reject a whole row. */
	assert(!cli_scrollback_append(&scrollback, CSTR_VIEW("a\xcd\xb8" "b")));
	cli_scrollback_test_render(&scrollback, 3, 1, false, &frame);
	assert(!strcmp(dstr_get(&frame), "\033[1;1Ha\xcd\xb8" "b"));
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
	cli_scrollback_test_render(&scrollback, 40, 4, false, &before);
	cli_scrollback_scroll(&scrollback, 20, true);
	cli_scrollback_test_render(&scrollback, 40, 4, false, &before);
	for (size_t i = 0; i < 10; ++i)
		assert(!cli_scrollback_append(&scrollback, CSTR_VIEW("new line\n")));
	cli_scrollback_test_render(&scrollback, 40, 4, false, &after);
	assert(cstr_view_equal(cstr_view_dstr(&before), cstr_view_dstr(&after)));
	assert(scrollback.unread);
	assert(scrollback.count == 10000);
	cli_scrollback_bottom(&scrollback);
	cli_scrollback_test_render(&scrollback, 40, 4, false, &after);
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
	cli_scrollback_test_render(&scrollback, 8, 3, true, &frame);
	CLI_SCROLLBACK_FRAME(&frame, CSTR_VIEW("\033[1;1H"), cli_style_sequence(CLI_STYLE_REASONING), CSTR_VIEW("abcdef"),
	                     cli_style_sequence(CLI_STYLE_USER_INPUT), CSTR_VIEW("界\033[2;1H"),
	                     cli_style_sequence(CLI_STYLE_USER_INPUT), CSTR_VIEW("        \033[3;1H"),
	                     cli_style_sequence(CLI_STYLE_USER_INPUT), CSTR_VIEW("x"),
	                     cli_style_sequence(CLI_STYLE_TEXT), CSTR_VIEW("!      "));
	cli_scrollback_test_render(&scrollback, 3, 1, true, &frame);
	CLI_SCROLLBACK_FRAME(&frame, CSTR_VIEW("\033[1;1H"), cli_style_sequence(CLI_STYLE_USER_INPUT), CSTR_VIEW("x"),
	                     cli_style_sequence(CLI_STYLE_TEXT), CSTR_VIEW("! "));
	cli_scrollback_scroll(&scrollback, 2, true);
	cli_scrollback_test_render(&scrollback, 3, 1, true, &frame);
	CLI_SCROLLBACK_FRAME(&frame, CSTR_VIEW("\033[1;1H"), cli_style_sequence(CLI_STYLE_USER_INPUT), CSTR_VIEW("界"),
	                     cli_style_sequence(CLI_STYLE_TEXT), CSTR_VIEW(" "));
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
	cli_scrollback_test_render(&scrollback, 2, 1, true, &frame);
	CLI_SCROLLBACK_FRAME(&frame, CSTR_VIEW("\033[1;1H"), cli_style_sequence(CLI_STYLE_USER_INPUT), CSTR_VIEW("x"),
	                     cli_style_sequence(CLI_STYLE_REASONING), CSTR_VIEW("x"));
	cli_scrollback_fini(&scrollback);
}

static void
cli_scrollback_test_pane (void)
{
	struct cli_scrollback scrollback;
	cli_scrollback_init(&scrollback);
	dstr_auto frame = dstr(nullptr);
	assert(cli_scrollback_render_pane(&scrollback, 0, 1, 4, 3, false, nullptr, &frame) == EINVAL);
	assert(cli_scrollback_render_pane(&scrollback, 1, 0, 4, 3, false, nullptr, &frame) == EINVAL);
	assert(cli_scrollback_render_pane(&scrollback, SIZE_MAX, 1, 4, 3, false, nullptr, &frame) == EINVAL);
	assert(cli_scrollback_render_pane(&scrollback, 1, SIZE_MAX, 4, 3, false, nullptr, &frame) == EINVAL);
	assert(!dstr_len(&frame));
	assert(!cli_scrollback_append_style(&scrollback, CSTR_VIEW("界éZ\nq"), CLI_STYLE_REASONING));
	assert(!cli_scrollback_render_pane(&scrollback, 3, 2, 4, 3, false, nullptr, &frame));
	assert(!strcmp(dstr_get(&frame), "\033[2;3H界éZ\033[3;3Hq   \033[4;3H    "));
	dstr_clear(&frame);
	assert(!cli_scrollback_render_pane(&scrollback, 3, 2, 4, 3, true, nullptr, &frame));
	CLI_SCROLLBACK_FRAME(&frame, CSTR_VIEW("\033[2;3H"), cli_style_sequence(CLI_STYLE_REASONING),
	                     CSTR_VIEW("界éZ\033[3;3H"), cli_style_sequence(CLI_STYLE_REASONING), CSTR_VIEW("q"),
	                     cli_style_sequence(CLI_STYLE_TEXT), CSTR_VIEW("   \033[4;3H"),
	                     cli_style_sequence(CLI_STYLE_TEXT), CSTR_VIEW("    "));
	cli_scrollback_fini(&scrollback);
	/* Neither an oversized wide character nor a leading combining character
	 * may draw into the neighboring column of a one-cell pane. */
	assert(!cli_scrollback_append(&scrollback, CSTR_VIEW("界\né\ń")));
	dstr_clear(&frame);
	assert(!cli_scrollback_render_pane(&scrollback, 5, 2, 1, 3, false, nullptr, &frame));
	assert(!strcmp(dstr_get(&frame), "\033[2;5H?\033[3;5Hé\033[4;5H?"));
	cli_scrollback_fini(&scrollback);
}

static void
cli_scrollback_test_cache (void)
{
	struct cli_scrollback scrollback;
	cli_scrollback_init(&scrollback);
	struct dstr cache[3] = {dstr(nullptr), dstr(nullptr), dstr(nullptr)};
	dstr_auto frame = dstr(nullptr);
	assert(!cli_scrollback_append_style(&scrollback, CSTR_VIEW("first\nsecond\nthird"), CLI_STYLE_REASONING));
	assert(!cli_scrollback_render_pane(&scrollback, 1, 1, 8, 3, true, cache, &frame));
	assert(dstr_len(&frame));
	dstr_clear(&frame);
	assert(!cli_scrollback_render_pane(&scrollback, 1, 1, 8, 3, true, cache, &frame));
	assert(!dstr_len(&frame));
	assert(!cli_scrollback_append_style(&scrollback, CSTR_VIEW("!"), CLI_STYLE_USER_INPUT));
	assert(!cli_scrollback_render_pane(&scrollback, 1, 1, 8, 3, true, cache, &frame));
	CLI_SCROLLBACK_FRAME(&frame, CSTR_VIEW("\033[3;6H"), cli_style_sequence(CLI_STYLE_USER_INPUT),
	                     CSTR_VIEW("!"), cli_style_sequence(CLI_STYLE_TEXT));
	dstr_clear(&frame);
	for (size_t i = 0; i < 3; ++i)
		dstr_clear(&cache[i]);
	assert(!cli_scrollback_render_pane(&scrollback, 1, 1, 8, 3, true, cache, &frame));
	dstr_auto full = dstr(nullptr);
	assert(!cli_scrollback_render_pane(&scrollback, 1, 1, 8, 3, true, nullptr, &full));
	assert(cstr_view_equal(cstr_view_dstr(&frame), cstr_view_dstr(&full)));
	for (size_t i = 0; i < 3; ++i)
		dstr_cleanup(&cache[i]);
	cli_scrollback_fini(&scrollback);
}

/** A deliberately small terminal model: positioned ASCII rows and SGR.
 * Any erase, scrolling or relative motion is a failure for pane output. */
static void
cli_scrollback_test_apply (char                   *screen,
                           size_t                  rows,
                           size_t                  columns,
                           struct cstr_view        text)
{
	size_t row = 0, column = 0;
	while (text.len) {
		if (text.data[0] != '\033') {
			assert(text.data[0] >= ' ' && text.data[0] <= '~');
			assert(row < rows && column < columns);
			screen[row * columns + column++] = text.data[0];
			text = cstr_view_slice(text, 1, SIZE_MAX);
			continue;
		}
		assert(text.len >= 3 && text.data[1] == '[');
		size_t end = 2;
		while (end < text.len && ((text.data[end] >= '0' && text.data[end] <= '9') || text.data[end] == ';'))
			++end;
		assert(end < text.len);
		if (text.data[end] == 'H') {
			unsigned next_row, next_column;
			assert(sscanf(text.data + 2, "%u;%uH", &next_row, &next_column) == 2);
			assert(next_row && next_column);
			row = next_row - 1;
			column = next_column - 1;
		} else
			assert(text.data[end] == 'm');
		text = cstr_view_slice(text, end + 1, SIZE_MAX);
	}
}

static void
cli_scrollback_test_neighbors (void)
{
	struct cli_scrollback left, right;
	cli_scrollback_init(&left);
	cli_scrollback_init(&right);
	assert(!cli_scrollback_append(&left, CSTR_VIEW("abcdeF")));
	assert(!cli_scrollback_append_style(&right, CSTR_VIEW("RIGHT"), CLI_STYLE_INFO));
	dstr_auto frame = dstr(nullptr);
	assert(!cli_scrollback_render_pane(&left, 2, 2, 5, 2, true, nullptr, &frame));
	assert(!cli_scrollback_render_pane(&right, 8, 2, 5, 2, true, nullptr, &frame));
	char screen[4 * 14];
	memset(screen, '#', sizeof screen);
	cli_scrollback_test_apply(screen, 4, 14, cstr_view_dstr(&frame));
	static char const expected[] =
		"##############"
		"#abcde#RIGHT##"
		"#F    #     ##"
		"##############";
	assert(sizeof expected - 1 == sizeof screen);
	assert(!memcmp(screen, expected, sizeof screen));
	cli_scrollback_fini(&left);
	cli_scrollback_fini(&right);
}

static void
cli_scrollback_test_pane_viewport (void)
{
	struct cli_scrollback chat, thoughts;
	cli_scrollback_init(&chat);
	cli_scrollback_init(&thoughts);
	for (size_t i = 0; i < 100; ++i) {
		dstr_auto line = dstr(nullptr);
		assert(dstr_printf(&line, "%03zu\n", i));
		assert(!cli_scrollback_append(&chat, cstr_view_dstr(&line)));
		assert(!cli_scrollback_append(&thoughts, cstr_view_dstr(&line)));
	}
	dstr_auto before = dstr(nullptr), after = dstr(nullptr);
	assert(!cli_scrollback_render_pane(&chat, 2, 3, 3, 2, false, nullptr, &before));
	assert(!cli_scrollback_render_pane(&thoughts, 6, 3, 3, 2, false, nullptr, &after));
	cli_scrollback_scroll(&chat, 20, true);
	dstr_clear(&before);
	assert(!cli_scrollback_render_pane(&chat, 2, 3, 3, 2, false, nullptr, &before));
	assert(!cli_scrollback_append(&chat, CSTR_VIEW("new\n")));
	assert(!cli_scrollback_append(&thoughts, CSTR_VIEW("new\n")));
	dstr_clear(&after);
	assert(!cli_scrollback_render_pane(&chat, 2, 3, 3, 2, false, nullptr, &after));
	assert(cstr_view_equal(cstr_view_dstr(&before), cstr_view_dstr(&after)));
	assert(chat.offset == 21 && chat.unread);
	dstr_clear(&after);
	assert(!cli_scrollback_render_pane(&thoughts, 6, 3, 3, 2, false, nullptr, &after));
	assert(!strcmp(dstr_get(&after), "\033[3;6Hnew\033[4;6H   "));
	assert(!thoughts.offset && !thoughts.unread);
	cli_scrollback_fini(&chat);
	cli_scrollback_fini(&thoughts);
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
	cli_scrollback_test_render(&scrollback, 80, 3, false, &before);
	cli_scrollback_scroll(&scrollback, 10, true);
	cli_scrollback_test_render(&scrollback, 80, 3, false, &before);
	memset(text, 'Z', 80);
	assert(!cli_scrollback_append(&scrollback, (struct cstr_view){text, 80}));
	cli_scrollback_test_render(&scrollback, 80, 3, false, &after);
	assert(cstr_view_equal(cstr_view_dstr(&before), cstr_view_dstr(&after)));
	assert(scrollback.offset == 11);
	/* A partial-row eviction must not rewrap the retained viewport. */
	assert(!cli_scrollback_append(&scrollback, CSTR_VIEW("Z")));
	cli_scrollback_test_render(&scrollback, 80, 3, false, &after);
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
	cli_scrollback_test_render(&scrollback, 80, 3, false, &frame);
	assert(dstr_len(&frame) < 300);
	cli_scrollback_fini(&scrollback);
	/* Repeated streaming redraws reuse the unchanged prefix's wrapping.
	 * Checkpoints also bound the work needed to reach the visible tail. */
	assert(!cli_scrollback_append(&scrollback, (struct cstr_view){text, 2 * 1024 * 1024}));
	for (size_t i = 0; i < 1024; ++i) {
		assert(!cli_scrollback_append(&scrollback, CSTR_VIEW("z")));
		cli_scrollback_test_render(&scrollback, 80, 3, false, &frame);
		assert(strchr(dstr_get(&frame), 'z'));
		assert(dstr_len(&frame) < 300);
	}
	/* A new width invalidates checkpoints without changing the transcript. */
	cli_scrollback_test_render(&scrollback, 17, 3, false, &frame);
	assert(strchr(dstr_get(&frame), 'z'));
	assert(dstr_len(&frame) < 100);
	free(text);
	text = nullptr;
	cli_scrollback_fini(&scrollback);
}

int
main (void)
{
	cli_scrollback_test_characters();
	cli_scrollback_test_wrapping();
	cli_scrollback_test_styles();
	cli_scrollback_test_pane();
	cli_scrollback_test_cache();
	cli_scrollback_test_neighbors();
	cli_scrollback_test_pane_viewport();
	cli_scrollback_test_viewport();
	cli_scrollback_test_long_line();
	cli_scrollback_test_prefix_eviction();
	puts("cli scrollback: UTF-8 wrapping, viewport, bounds and streaming redraw: PASS");
	return 0;
}

#undef CLI_SCROLLBACK_FRAME
