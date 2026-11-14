/** @file Unit tests for upstream requests and events (src/ai.c). */
#include <json-c/json.h>

#include "ai.h"
#include "check.h"
#include "dstr.h"
#include "jb.h"

/* Durable job journals persist these values. Append providers rather
 * than renumbering ones that an earlier executable may have written.
 */
static_assert(AI_NO_PROVIDER == 0 && AI_LLAMA == 1 && AI_OPENAI == 2 &&
              AI_ANTHROPIC == 3 && AI_GOOGLE == 4 && AI_DEEPINFRA == 5 &&
              AI_OPENROUTER == 6);

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
	struct json_object *ev = json_tokener_parse(json);
	if (!ev)
		return false;
	struct ai_sink const sink = ai_sink(.text = ignore_text);
	struct ai_reader reader = {};
	struct ai_event out = {};
	converted = 0;
	(void)ai_event(api, &reader, ev, &sink, &out);
	json_object_put(ev);
	return !converted;
}

/* Like mcp_lent_text(), serialize an otherwise empty array only when the
 * request reaches the wire. Wrapping must not lose this serializer.
 */
static int
lazy_tools (struct json_object *obj,
            struct printbuf    *pb,
            int                 level,
            int                 flags)
{
	(void)obj;
	(void)level;
	(void)flags;
	static char const text[] = "[{\"type\":\"function\",\"name\":\"git\","
	                           "\"parameters\":{\"type\":\"object\"}}]";
	return printbuf_memappend(pb, text, sizeof text - 1) < 0 ? -1 : 0;
}

static void
chatgpt_request (void)
{
	struct json_object *extra = json_tokener_parse(
		"{\"store\":true,\"stream\":false,\"background\":true,"
		"\"conversation\":\"c\",\"previous_response_id\":\"r\","
		"\"max_output_tokens\":10,\"max_tool_calls\":2,\"metadata\":{},"
		"\"moderation\":{},\"multi_agent\":true,\"prompt\":{},"
		"\"prompt_cache_retention\":\"24h\",\"safety_identifier\":\"s\","
		"\"temperature\":0.5,\"top_logprobs\":1,\"top_p\":0.9,"
		"\"truncation\":\"auto\",\"user\":\"u\","
		"\"reasoning\":{\"effort\":\"high\"},\"tool_choice\":\"auto\"}");
	struct json_object *system = json_object_new_string("Be precise.");
	struct json_object *prompt = json_object_new_string("Inspect this diff.");
	struct json_object *history = json_tokener_parse(
		"[{\"role\":\"user\",\"content\":\"Read the repository.\"},"
		"{\"role\":\"assistant\",\"content\":\"Ready.\"}]");
	struct json_object *tools = json_object_new_array();
	json_object_set_serializer(tools, lazy_tools, nullptr, nullptr);
	struct ai_endpoint ep = {
		.api = AI_RESPONSES, .provider = AI_CHATGPT, .model = "gpt-6-astra",
		.extra = extra, .temperature = jb_get(extra, "temperature"),
		.max_tokens = 512, .background = 1,
	};
	CHECK(!ai_chatgpt_check(extra));
	CHECK(!ai_background(&ep));
	struct json_object *request = ai_request(&ep, system, prompt, history, tools);
	CHECK(request);
	struct json_object *wire = request ? json_tokener_parse(jb_wire(request).data) : nullptr;
	CHECK(wire);
	static char const *const omitted[] = {
		"background", "conversation", "previous_response_id", "max_output_tokens",
		"max_tool_calls", "metadata", "moderation", "multi_agent", "prompt",
		"prompt_cache_retention", "safety_identifier", "temperature", "top_logprobs",
		"top_p", "truncation", "user",
	};
	for (size_t k = 0; k < array_size(omitted); ++k)
		CHECK(!jb_get(wire, omitted[k]));
	CHECK(json_object_is_type(jb_get(wire, "store"), json_type_boolean));
	CHECK(!json_object_get_boolean(jb_get(wire, "store")));
	CHECK(json_object_get_boolean(jb_get(wire, "stream")));
	CHECK(jb_is(jb_get(wire, "model"), "gpt-6-astra"));
	CHECK(jb_is(jb_get(wire, "instructions"), "Be precise."));
	CHECK(jb_is(jb_get(jb_get(wire, "reasoning"), "effort"), "high"));
	CHECK(jb_len(jb_get(wire, "input")) == 3);
	CHECK(jb_is(jb_get(jb_at(jb_get(wire, "input"), 2), "content"), "Inspect this diff."));
	struct json_object *ns = jb_at(jb_get(wire, "tools"), 0);
	CHECK(jb_is(jb_get(ns, "type"), "namespace"));
	CHECK(jb_is(jb_get(ns, "name"), "askai"));
	CHECK(jb_len(jb_get(ns, "tools")) == 1);
	CHECK(jb_is(jb_get(jb_at(jb_get(ns, "tools"), 0), "name"), "git"));
	char const *last = nullptr;
	struct json_object_iter it;
	json_object_object_foreachC(request, it)
		last = it.key;
	CHECK(last && !strcmp(last, "input"));
	json_object_put(wire);
	json_object_put(request);

	/* The shared Responses encoder keeps the API-key provider's contract. */
	ep.provider = AI_OPENAI;
	CHECK(ai_background(&ep));
	request = ai_request(&ep, system, prompt, history, tools);
	CHECK(request);
	wire = request ? json_tokener_parse(jb_wire(request).data) : nullptr;
	CHECK(json_object_get_boolean(jb_get(wire, "background")));
	CHECK(json_object_get_boolean(jb_get(wire, "store")));
	CHECK(jb_get(wire, "temperature"));
	CHECK(jb_int(jb_get(wire, "max_output_tokens"), 0) == 10);
	CHECK(jb_is(jb_get(jb_at(jb_get(wire, "tools"), 0), "type"), "function"));
	json_object_put(wire);
	json_object_put(request);

	ep.provider = AI_CHATGPT;
	ep.extra = nullptr;
	request = ai_request(&ep, system, prompt, nullptr, nullptr);
	CHECK(request && !jb_get(request, "tools"));
	CHECK(!jb_get(request, "max_output_tokens"));
	json_object_put(request);
	CHECK(jb_len(tools) == 0);
	CHECK(json_object_get_boolean(jb_get(extra, "store")));
	json_object_put(tools);
	json_object_put(history);
	json_object_put(prompt);
	json_object_put(system);
	json_object_put(extra);
}

static void
chatgpt_extra (void)
{
	static char const *const invalid[] = {
		"{\"input\":null}", "{\"input\":[]}", "{\"tools\":null}", "{\"tools\":[]}",
		"{\"tool_choice\":{\"type\":\"function\",\"name\":\"git\"}}",
		"{\"tool_choice\":true}", "{\"instructions\":{}}",
		"{\"include\":\"reasoning.encrypted_content\"}", "{\"include\":[null]}",
	};
	CHECK(!ai_chatgpt_check(nullptr));
	for (size_t k = 0; k < array_size(invalid); ++k) {
		struct json_object *extra = json_tokener_parse(invalid[k]);
		CHECK(ai_chatgpt_check(extra));
		json_object_put(extra);
	}
	struct json_object *extra = json_tokener_parse(
		"{\"tool_choice\":\"required\",\"instructions\":\"Keep it brief.\","
		"\"include\":[\"reasoning.encrypted_content\"]}");
	CHECK(!ai_chatgpt_check(extra));
	json_object_put(extra);
}

struct replay_test {
	struct dstr replay;
	unsigned calls;
};

static int
replay_text (void             *user,
             struct cstr_view  text)
{
	struct replay_test *test = user;
	return dstr_cat_n(&test->replay, text.data, text.len) ? 0 : ENOMEM;
}

static int
replay_call (void                 *user,
             struct ai_call const *call)
{
	struct replay_test *test = user;
	++test->calls;
	CHECK(cstr_view_equal(call->id, cstr_view("call_1")));
	CHECK(cstr_view_equal(call->name, cstr_view("git")));
	CHECK(cstr_view_equal(call->args, cstr_view("{\"op\":\"diff\"}")));
	return 0;
}

static void
chatgpt_replay (void)
{
	struct replay_test test = { .replay = dstr("[{}") };
	struct ai_sink const sink = ai_sink(.text = ignore_text, .call = replay_call,
	                                    .replay = replay_text, .user = &test);
	struct ai_reader reader = {};
	struct ai_event out = {};
	struct json_object *ev = json_tokener_parse(
		"{\"type\":\"response.output_item.done\",\"output_index\":0,\"item\":{"
		"\"type\":\"reasoning\",\"id\":\"rs_1\",\"summary\":[],"
		"\"encrypted_content\":\"opaque-content\"}}");
	CHECK(!ai_event(AI_RESPONSES, &reader, ev, &sink, &out));
	CHECK(out.flags & AI_EV_THOUGHT_END);
	json_object_put(ev);
	ev = json_tokener_parse(
		"{\"type\":\"response.output_item.done\",\"output_index\":1,\"item\":{"
		"\"type\":\"function_call\",\"name\":\"git\",\"namespace\":\"askai\","
		"\"call_id\":\"call_1\",\"arguments\":\"{\\\"op\\\":\\\"diff\\\"}\"}}");
	CHECK(!ai_event(AI_RESPONSES, &reader, ev, &sink, &out));
	CHECK(test.calls == 1);
	json_object_put(ev);
	CHECK(dstr_cat(&test.replay, "]"));
	struct json_object *replayed = json_tokener_parse(dstr_get(&test.replay));
	CHECK(jb_len(replayed) == 3);
	CHECK(jb_is(jb_get(jb_at(replayed, 1), "encrypted_content"), "opaque-content"));
	CHECK(jb_is(jb_get(jb_at(replayed, 2), "namespace"), "askai"));
	CHECK(jb_is(jb_get(jb_at(replayed, 2), "call_id"), "call_1"));
	json_object_put(replayed);
	dstr_cleanup(&test.replay);

	ev = json_tokener_parse("{\"type\":\"response.failed\",\"response\":{\"error\":{"
	                       "\"code\":\"subscription_sharing_usage_limit_exceeded\","
	                       "\"message\":\"ChatGPT plan usage limit exceeded.\"}}}");
	CHECK(!ai_event(AI_RESPONSES, &reader, ev, &sink, &out));
	CHECK(out.error && !strcmp(out.error, "ChatGPT plan usage limit exceeded."));
	json_object_put(ev);
}

int
main (void)
{
	chatgpt_request();
	chatgpt_extra();
	chatgpt_replay();
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

	return check_report("ai");
}
