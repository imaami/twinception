/** @file Unit tests for the JSON helpers (src/jb.c). */
#include <errno.h>
#include <json-c/json.h>
#include <stdio.h>
#include <string.h>

#include "jb.h"

static unsigned failures;

#define CHECK(cond) \
        ((cond) ? (void)0 \
                : (void)(++failures, fprintf(stderr, "%s:%d: CHECK(%s) failed\n", \
                                             __FILE__, __LINE__, #cond)))

/* Whether json-c's tokener constructors fail, as when memory runs out. */
static bool no_tokener;

extern struct json_tokener *
__real_json_tokener_new (void);

extern struct json_tokener *
__real_json_tokener_new_ex (int depth);

struct json_tokener *
__wrap_json_tokener_new (void)
{
	return no_tokener ? nullptr : __real_json_tokener_new();
}

struct json_tokener *
__wrap_json_tokener_new_ex (int depth)
{
	return no_tokener ? nullptr : __real_json_tokener_new_ex(depth);
}

/* The result of parsing @p text with jb_parser_step(), @p bytes at most a
 * step. @p steps gets the number of steps.
 */
static int
parse (char const *text,
       size_t      bytes,
       unsigned   *steps)
{
	struct jb_pool pool = { .scalar = 16 };
	struct jb_parser parser = {};
	size_t len = strlen(text);
	int e;
	*steps = 0;
	do
		++*steps;
	while ((e = jb_parser_step(&parser, &pool, text, len, bytes, UINT32_MAX)) == EAGAIN);
	struct json_object *root = jb_parser_take(&parser);
	CHECK(!root == (e != 0));
	CHECK(parser.error == e);
	json_object_put(root);
	jb_parser_fini(&parser, &pool, nullptr);
	(void)jb_pool_release(&pool, 0);
	return e;
}

/* Whether jb_int() and jb_uint() give @p want_int and @p want_uint for the
 * value of "v" in @p json, with -7 and 7 as their defaults.
 */
static bool
integer (char const *json,
         int64_t     want_int,
         uint64_t    want_uint)
{
	struct json_object *obj = json_tokener_parse(json);
	struct json_object *v = jb_get(obj, "v");
	bool ok = jb_int(v, -7) == want_int && jb_uint(v, 7) == want_uint;
	json_object_put(obj);
	return ok;
}

int
main (void)
{
	unsigned steps;

	/* Only a JSON integer is read; anything else gives the default. */
	CHECK(integer("{\"v\": 42}", 42, 42));
	CHECK(integer("{\"v\": -42}", -42, 0));
	CHECK(integer("{\"v\": 18446744073709551615}", INT64_MAX, UINT64_MAX));
	CHECK(integer("{\"v\": NaN}", -7, 7));
	CHECK(integer("{\"v\": Infinity}", -7, 7));
	CHECK(integer("{\"v\": 1.5}", -7, 7));
	CHECK(integer("{\"v\": \"42\"}", -7, 7));
	CHECK(integer("{\"v\": true}", -7, 7));
	CHECK(integer("{}", -7, 7));
	CHECK(jb_int(nullptr, -7) == -7 && jb_uint(nullptr, 7) == 7);

	/* A nullptr value fails the set or the push. The failure frees the
	 * container. With a nullptr container, the call returns nullptr and
	 * frees the value. */
	struct json_object *obj = jb_set_or_free(json_object_new_object(), "a",
	                                         json_object_new_int(1));
	CHECK(jb_int(jb_get(obj, "a"), 0) == 1);
	CHECK(!jb_set_or_free(obj, "b", nullptr));
	CHECK(!jb_set_or_free(nullptr, "a", json_object_new_int(1)));
	struct json_object *arr = jb_push_or_free(jb_list(json_object_new_int(1)),
	                                          json_object_new_int(2));
	CHECK(jb_len(arr) == 2 && jb_int(jb_at(arr, 1), 0) == 2 && !jb_at(arr, 2));
	CHECK(!jb_push_or_free(arr, nullptr));
	CHECK(!jb_push_or_free(nullptr, json_object_new_int(1)));
	CHECK(!jb_list(nullptr));

	/* jb_len() returns 0 for a value that is not an array. */
	struct json_object *text = json_object_new_string("abc");
	CHECK(jb_len(text) == 0 && jb_len(nullptr) == 0);
	json_object_put(text);

	/* jb_escape() writes the inside of the string that json-c writes. */
	static char const raw[] = "a\"\\\n\x01\x1f/\x7f\xc3\xa4";
	char escaped[6 * sizeof raw];
	struct json_object *str = json_object_new_string_len(raw, sizeof raw - 1);
	size_t wire = 0;
	char const *json = json_object_to_json_string_length(str, JB_WIRE, &wire);
	size_t n = jb_escape(escaped, raw, sizeof raw - 1);
	CHECK(json && n == wire - 2 && !memcmp(escaped, json + 1, n));
	json_object_put(str);
	CHECK(!jb_escape(escaped, nullptr, 1) && !jb_escape(nullptr, raw, 1));

	/* One JSON value, whole or a slice at a time. */
	CHECK(parse("{\"a\": [1, 2, \"three\"]}", SIZE_MAX, &steps) == 0 && steps == 1);
	CHECK(parse("{\"a\": [1, 2, \"three\"]}", 4, &steps) == 0 && steps > 1);
	CHECK(parse(" 123 \n", 4, &steps) == 0);
	CHECK(parse("123", SIZE_MAX, &steps) == 0);

	/* Text that is not one JSON value, or breaks the pool's limits. */
	CHECK(parse("{\"a\": ", SIZE_MAX, &steps) == EINVAL);
	CHECK(parse("{\"a\": x}", SIZE_MAX, &steps) == EINVAL);
	CHECK(parse("{} {}", SIZE_MAX, &steps) == EINVAL);
	CHECK(parse("[] x", 4, &steps) == EINVAL);
	CHECK(parse("", SIZE_MAX, &steps) == EINVAL);
	CHECK(parse("12345678901234567890", SIZE_MAX, &steps) == EINVAL);

	/* No tokener. */
	no_tokener = true;
	CHECK(parse("{}", SIZE_MAX, &steps) == ENOMEM && steps == 1);
	no_tokener = false;

	if (failures)
		fprintf(stderr, "jb: %u checks failed\n", failures);
	else
		puts("jb: all checks passed");
	return failures != 0;
}
