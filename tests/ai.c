/** @file Unit tests for the upstream event parser (src/ai.c). */
#include <json-c/json.h>
#include <stdio.h>
#include <string.h>

#include "ai.h"

static unsigned failures;

#define CHECK(cond) \
        ((cond) ? (void)0 \
                : (void)(++failures, fprintf(stderr, "%s:%d: CHECK(%s) failed\n", \
                                             __FILE__, __LINE__, #cond)))

/* The number of values that were not JSON integers and that ai.c passed to
 * json_object_get_int64(). json-c parses NaN as a double, and converting
 * NaN to int64_t is undefined behavior.
 */
static unsigned converted;

extern int64_t
__real_json_object_get_int64 (struct json_object const *obj);

int64_t
__wrap_json_object_get_int64 (struct json_object const *obj)
{
	converted += obj && !json_object_is_type(obj, json_type_int);
	return __real_json_object_get_int64(obj);
}

static int
ignore_text (void             *user,
             struct cstr_view  text)
{
	(void)user;
	(void)text;
	return 0;
}

/* Whether ai_event() reads the event @p json of @p api without converting a
 * value that is not a JSON integer. The event is parsed as home parses
 * events, by a tokener that is not strict.
 */
static bool
integers_only (enum ai_api  api,
               char const *json)
{
	struct json_tokener *tok = json_tokener_new();
	if (!tok)
		return false;
	struct json_object *ev = json_tokener_parse_ex(tok, json, (int)strlen(json));
	json_tokener_free(tok);
	if (!ev)
		return false;
	struct ai_sink const sink = { .text = ignore_text };
	struct ai_reader reader = {};
	struct ai_event out = {};
	converted = 0;
	(void)ai_event(api, &reader, ev, &sink, &out);
	json_object_put(ev);
	return !converted;
}

int
main (void)
{
	/* A NaN where each API has a token count, or a choice's index. */
	CHECK(integers_only(AI_CHAT,
	      "{\"choices\":[{\"index\":NaN,\"delta\":{}}]}"));
	CHECK(integers_only(AI_CHAT,
	      "{\"choices\":[],\"usage\":{\"prompt_tokens\":NaN,"
	      "\"completion_tokens\":NaN}}"));
	CHECK(integers_only(AI_RESPONSES,
	      "{\"type\":\"response.completed\",\"response\":{\"usage\":"
	      "{\"input_tokens\":NaN,\"output_tokens\":NaN}}}"));
	CHECK(integers_only(AI_MESSAGES,
	      "{\"type\":\"message_start\",\"message\":{\"usage\":"
	      "{\"input_tokens\":NaN,\"cache_creation_input_tokens\":NaN,"
	      "\"cache_read_input_tokens\":NaN}}}"));
	CHECK(integers_only(AI_MESSAGES,
	      "{\"type\":\"message_delta\",\"usage\":{\"output_tokens\":NaN}}"));
	CHECK(integers_only(AI_GENERATE,
	      "{\"candidates\":[],\"usageMetadata\":{\"totalTokenCount\":NaN}}"));
	CHECK(integers_only(AI_INTERACTIONS,
	      "{\"event_type\":\"interaction.completed\",\"interaction\":"
	      "{\"status\":\"completed\",\"usage\":{\"total_input_tokens\":NaN,"
	      "\"total_output_tokens\":1,\"total_tokens\":50}}}"));

	if (failures)
		fprintf(stderr, "ai: %u checks failed\n", failures);
	else
		puts("ai: all checks passed");
	return failures != 0;
}
