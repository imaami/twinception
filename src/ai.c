/** @file ai.c
 *
 * Upstream AI APIs: request bodies and answer parsing, whole or streamed.
 */
#include <errno.h>
#include <stdckdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "ai.h"
#include "dstr.h"
#include "jb.h"
#include "util.h"

/** Anthropic requires max_tokens; this fits every model's output limit. */
enum { MESSAGES_MAX_TOKENS = 4096 };

/* Each API's stop reason for an answer the output token limit cut. */
static char const *const limits[] = {
	[AI_CHAT] = "length",
	[AI_RESPONSES] = "max_output_tokens",
	[AI_MESSAGES] = "max_tokens",
	[AI_GENERATE] = "MAX_TOKENS",
	[AI_INTERACTIONS] = "incomplete",
};

/* The type of the open step, in Gemini's Interactions, or the end of the
 * answer, after which no event changes the replay (struct ai_reader's
 * kind). An open thought is not in the replay until it has a summary item
 * or a signature.
 */
enum { INTERACTIONS_THOUGHT, INTERACTIONS_OUTPUT, INTERACTIONS_ENDED };

/* The state of the summary array of the open thought (struct ai_reader's
 * summary).
 */
enum { SUMMARY_NONE, SUMMARY_OPEN, SUMMARY_CLOSED };

static struct json_object *
ai_message (char const         *role,
            struct json_object *content)
{
	return jb_object("role", json_object_new_string(role),
	                 "content", json_object_get(content),
	                 nullptr);
}

static struct json_object *
ai_generate_content (char const         *role,
                     struct json_object *text)
{
	struct json_object *parts = jb_list(jb_object(
		"text", json_object_get(text), nullptr));
	return role ? jb_object("role", json_object_new_string(role),
	                        "parts", parts, nullptr)
	            : jb_object("parts", parts, nullptr);
}

/* A visible chat message, in the upstream API's own format. */
static struct json_object *
ai_input (enum ai_api        api,
          char const        *role,
          struct json_object *text)
{
	if (api == AI_GENERATE)
		return ai_generate_content(!strcmp(role, "assistant") ? "model" : role, text);
	if (api != AI_INTERACTIONS)
		return ai_message(role, text);
	struct json_object *content = jb_object("type", json_object_new_string("text"),
	                                        "text", json_object_get(text), nullptr);
	return jb_object("type", json_object_new_string(!strcmp(role, "assistant")
	                                               ? "model_output" : "user_input"),
	                 "content", jb_list(content), nullptr);
}

/* Earlier user and assistant messages, then the new prompt. Only chat
 * completions includes the system message in this array. The caller has
 * validated the history's roles and plain text contents.
 */
static struct json_object *
ai_conversation (enum ai_api        api,
                 struct json_object *system,
                 struct json_object *history,
                 struct json_object *prompt)
{
	struct json_object *messages = json_object_new_array();
	if (system)
		messages = jb_push_or_free(messages, ai_message("system", system));
	size_t n = jb_len(history);
	for (size_t k = 0; messages && k < n; ++k) {
		struct json_object *message = jb_at(history, k);
		messages = jb_push_or_free(messages, ai_input(api, jb_text(jb_get(message, "role")),
		                                              jb_get(message, "content")));
	}
	return jb_push_or_free(messages, ai_input(api, "user", prompt));
}

/* A copy of @p val, which the configuration holds: every loop reads it, so no
 * request may take a reference, whose count is not atomic. nullptr if out of
 * memory.
 */
static struct json_object *
ai_copy (struct json_object *val)
{
	struct json_object *dup = nullptr;
	return json_object_deep_copy(val, &dup, nullptr) ? nullptr : dup;
}

/* Deep-merge @p src into @p dst; a null value removes the key. */
static int
ai_merge (struct json_object *dst,
          struct json_object *src)
{
	struct json_object_iter it;
	json_object_object_foreachC(src, it) {
		struct json_object *cur = jb_get(dst, it.key);
		if (!it.val) {
			json_object_object_del(dst, it.key);
		} else if (json_object_is_type(it.val, json_type_object) &&
		           json_object_is_type(cur, json_type_object)) {
			if (ai_merge(cur, it.val))
				return ENOMEM;
		} else if (jb_set(dst, it.key, ai_copy(it.val))) {
			return ENOMEM;
		}
	}
	return 0;
}

/* Write the token limit under @p max_key, unless @p max_tokens is not
 * positive, and the temperature into @p root. If @p config is not nullptr,
 * write them into a new object, @p root's member @p config, which is added
 * only if one of them is set. Then merge --extra into @p root.
 */
static int
ai_request_common (struct ai_endpoint const *ep,
                   struct json_object       *root,
                   char const               *config,
                   char const               *max_key,
                   long                      max_tokens)
{
	struct json_object *options = root;
	if (config && (max_tokens > 0 || ep->temperature)) {
		options = json_object_new_object();
		if (jb_set(root, config, options))
			return ENOMEM;
	}
	if ((max_tokens > 0 &&
	     jb_set(options, max_key, json_object_new_int64(max_tokens))) ||
	    (ep->temperature && jb_set(options, "temperature", ai_copy(ep->temperature))) ||
	    (ep->extra && ai_merge(root, ep->extra)))
		return ENOMEM;
	return 0;
}

/* How each API's body is built around its conversation, the member that
 * ai_format() names: the member of the system prompt, which chat
 * completions has in the conversation instead; the object of the
 * generation options, or the body itself; the member of the token limit,
 * and its default; and which other members it has.
 */
enum {
	BODY_MODEL  = 1 << 0,  /* the model, first */
	BODY_USAGE  = 1 << 1,  /* with tools, the usage in the stream */
	BODY_STORE  = 1 << 2,  /* "store", false */
	BODY_KEEP   = 1 << 3,  /* "store", true with native background
	                          execution, which needs storage */
	BODY_STREAM = 1 << 4,  /* "stream", after --extra is merged, so that the
	                          answer is always streamed */
	BODY_PARTS  = 1 << 5,  /* the system prompt in an object with an array
	                          of parts */
};

static struct ai_body {
	char const *system;
	char const *options;
	char const *max;
	int32_t     max_default;
	uint32_t    flags;
} const ai_bodies[] = {
	[AI_CHAT]         = { nullptr, nullptr, "max_tokens", 0,
	                      BODY_MODEL | BODY_USAGE | BODY_STREAM },
	[AI_RESPONSES]    = { "instructions", nullptr, "max_output_tokens", 0,
	                      BODY_MODEL | BODY_STORE | BODY_STREAM },
	[AI_MESSAGES]     = { "system", nullptr, "max_tokens", MESSAGES_MAX_TOKENS,
	                      BODY_MODEL | BODY_STREAM },
	[AI_GENERATE]     = { "systemInstruction", "generationConfig", "maxOutputTokens", 0,
	                      BODY_PARTS },
	[AI_INTERACTIONS] = { "system_instruction", "generation_config",
	                      "max_output_tokens", 0,
	                      BODY_MODEL | BODY_STORE | BODY_KEEP | BODY_STREAM },
};

/* Move the conversation, @p root's member @p key, to its end, after what
 * --extra added, so that turns may follow it, unless --extra took it away.
 */
static int
ai_request_last (struct json_object *root,
                 char const         *key)
{
	struct json_object *val = jb_get(root, key);
	if (!val)
		return 0;
	json_object_get(val);
	json_object_object_del(root, key);
	return jb_set(root, key, val);
}

struct json_object *
ai_request (struct ai_endpoint const *ep,
            struct json_object       *system,
            struct json_object       *prompt,
            struct json_object       *history,
            struct json_object       *tools)
{
	struct json_object *root = json_object_new_object();
	if (!root)
		return nullptr;
	struct ai_body const *b = &ai_bodies[ep->api];
	char const *conversation = ai_format(ep->api)->conversation;
	bool background = ai_background(ep);
	if (b->flags & BODY_MODEL && ep->model &&
	    jb_set(root, "model", json_object_new_string(ep->model)))
		goto fail;
	/* Chat completions has the system prompt as a message in the
	 * conversation. The other APIs have it in the member b->system. */
	if (b->system) {
		if (system && jb_set(root, b->system, b->flags & BODY_PARTS
		                                      ? ai_generate_content(nullptr, system)
		                                      : json_object_get(system)))
			goto fail;
		system = nullptr;
	}
	if (jb_set(root, conversation, ai_conversation(ep->api, system, history, prompt)) ||
	    (tools && (jb_set(root, "tools", json_object_get(tools)) ||
	               (b->flags & BODY_USAGE &&
	                jb_set(root, "stream_options",
	                       jb_object("include_usage", json_object_new_boolean(1),
	                                 nullptr))))) ||
	    (b->flags & BODY_STORE &&
	     jb_set(root, "store", json_object_new_boolean(b->flags & BODY_KEEP && background))) ||
	    ai_request_common(ep, root, b->options, b->max,
	                      ep->max_tokens > 0 ? ep->max_tokens : b->max_default) ||
	    (background && jb_set(root, "background", json_object_new_boolean(1))) ||
	    (b->flags & BODY_STREAM && jb_set(root, "stream", json_object_new_boolean(1))) ||
	    ai_request_last(root, conversation))
		goto fail;
	return root;
fail:
	json_object_put(root);
	return nullptr;
}

/* Append the text of @p val to @p text if @p val is a string. */
static int
ai_append (struct dstr        *text,
           struct json_object *val)
{
	struct cstr_view str = jb_view(val);
	return str.data && !dstr_cat_n(text, str.data, str.len) ? ENOMEM : 0;
}

/* The text of chat completions' answer @p root, into @p text, and its stop
 * reason in @p *reason.
 */
static int
ai_chat_answer (struct json_object  *root,
                struct dstr         *text,
                char const         **reason)
{
	struct json_object *choice = jb_at(jb_get(root, "choices"), 0);
	struct json_object *msg = jb_get(choice, "message");
	struct json_object *content = jb_get(msg, "content");
	*reason = jb_text(jb_get(choice, "finish_reason"));

	/* Some servers send content as an array of text parts. */
	int e = ai_append(text, content);
	struct json_object *part;
	for (size_t i = 0; !e && (part = jb_at(content, i)); ++i)
		e = ai_append(text, jb_get(part, "text"));
	if (e || dstr_len(text))
		return e;
	return ai_append(text, jb_get(msg, "refusal"));
}

static int
ai_responses_answer (struct json_object  *root,
                     struct dstr         *text,
                     char const         **reason)
{
	*reason = jb_text(jb_get(jb_get(root, "incomplete_details"), "reason"));
	struct json_object *output = jb_get(root, "output");
	int e = 0;
	struct json_object *item;
	for (size_t i = 0; !e && (item = jb_at(output, i)); ++i) {
		if (!jb_is(jb_get(item, "type"), "message"))
			continue;
		struct json_object *content = jb_get(item, "content");
		struct json_object *part;
		for (size_t j = 0; !e && (part = jb_at(content, j)); ++j) {
			bool refusal = jb_is(jb_get(part, "type"), "refusal");
			e = ai_append(text, jb_get(part, refusal ? "refusal" : "text"));
		}
	}
	return e;
}

static int
ai_messages_answer (struct json_object  *root,
                    struct dstr         *text,
                    char const         **reason)
{
	*reason = jb_text(jb_get(root, "stop_reason"));
	struct json_object *content = jb_get(root, "content");
	int e = 0;
	struct json_object *block;
	for (size_t i = 0; !e && (block = jb_at(content, i)); ++i)
		if (jb_is(jb_get(block, "type"), "text"))
			e = ai_append(text, jb_get(block, "text"));
	return e;
}

static int
ai_generate_answer (struct json_object  *root,
                    struct dstr         *text,
                    char const         **reason)
{
	struct json_object *candidate = jb_at(jb_get(root, "candidates"), 0);
	struct json_object *parts = jb_get(jb_get(candidate, "content"), "parts");
	*reason = jb_text(candidate ? jb_get(candidate, "finishReason")
	                            : jb_get(jb_get(root, "promptFeedback"), "blockReason"));
	int e = 0;
	struct json_object *part;
	for (size_t i = 0; !e && (part = jb_at(parts, i)); ++i)
		if (!json_object_get_boolean(jb_get(part, "thought")))
			e = ai_append(text, jb_get(part, "text"));
	return e;
}

/* The text of the model_output steps of Gemini's Interactions' answer
 * @p root, into @p text, and its status, as its stop reason, in
 * @p *reason.
 */
static int
ai_interactions_answer (struct json_object  *root,
                        struct dstr         *text,
                        char const         **reason)
{
	*reason = jb_text(jb_get(root, "status"));
	struct json_object *steps = jb_get(root, "steps");
	int e = 0;
	struct json_object *step;
	for (size_t i = 0; !e && (step = jb_at(steps, i)); ++i) {
		if (!jb_is(jb_get(step, "type"), "model_output"))
			continue;
		struct json_object *content = jb_get(step, "content");
		struct json_object *item;
		for (size_t j = 0; !e && (item = jb_at(content, j)); ++j)
			if (jb_is(jb_get(item, "type"), "text"))
				e = ai_append(text, jb_get(item, "text"));
	}
	return e;
}

static char const no_answer[] = "The model returned no answer.";
#define no_answer_reason "The model returned no answer (stop reason: %s)."

size_t
ai_no_answer (char       *buf,
              size_t      size,
              char const *reason)
{
	if (!reason)
		return util_format(buf, size, "%s", no_answer);
	return util_format(buf, size, no_answer_reason, reason);
}

/* Pass @p text, a piece of the text of the answer, to sink->text, unless
 * text.data is nullptr because the JSON value was not a string.
 */
static int
ai_put_text (struct ai_sink const *sink,
             struct cstr_view      text)
{
	return text.data ? sink->text(sink->user, text) : 0;
}

/* Pass @p text, a piece of the thinking of the model, to sink->thought,
 * unless sink->thought or text.data is nullptr. Set out->thought if
 * @p text is not empty.
 */
static int
ai_put_thought (struct ai_sink const *sink,
                struct cstr_view      text,
                struct ai_event      *out)
{
	out->thought |= text.len != 0;
	return text.data && sink->thought ? sink->thought(sink->user, text) : 0;
}

/* @p val as a call's index, or a content block's: AI_CALL_NEXT if it is
 * none.
 */
static uint32_t
ai_index (struct json_object *val)
{
	int64_t at = json_object_is_type(val, json_type_int) ? json_object_get_int64(val)
	                                                     : -1;
	return at >= 0 && at < AI_CALL_NEXT ? (uint32_t)at : AI_CALL_NEXT;
}

/* Return the sum of @p prompt, the token count of a request, and @p answer,
 * the token count of its answer. Return UINT32_MAX if the sum is larger
 * than UINT32_MAX. Return 0, which means that the count is not known, if
 * @p prompt is not positive, if @p answer is negative, or if the sum
 * overflows int64_t.
 */
static uint32_t
ai_used (int64_t prompt,
         int64_t answer)
{
	int64_t n;
	if (prompt <= 0 || answer < 0 || ckd_add(&n, prompt, answer))
		return 0;
	return n < UINT32_MAX ? (uint32_t)n : UINT32_MAX;
}

/* Return @p val as a token count if it is a JSON integer, else 0. json-c
 * also parses NaN, which json_object_get_int64() cannot convert. ai_used()
 * treats a request count of 0 as not known and an answer count of 0 as 0.
 */
static int64_t
ai_count (struct json_object *val)
{
	return json_object_is_type(val, json_type_int) ? json_object_get_int64(val) : 0;
}

/* The place of @p type, an event's or an item's, among the @p n @p names; @p n
 * if it is none of them.
 */
static size_t
ai_kind (struct json_object *type,
         char const *const  *names,
         size_t              n)
{
	size_t i = 0;
	while (i < n && !jb_is(type, names[i]))
		++i;
	return i;
}

/* Set @p *json to the JSON text of @p val, or to an empty view if @p val is
 * nullptr. Return 0, or ENOMEM if json-c cannot allocate the text.
 */
static int
ai_json_text (struct json_object *val,
              struct cstr_view   *json)
{
	if (!val) {
		*json = (struct cstr_view){};
		return 0;
	}
	json->data = json_object_to_json_string_length(val, JB_WIRE, &json->len);
	return json->data ? 0 : ENOMEM;
}

/* Pass @p json, a piece of JSON text, to sink->replay, unless
 * sink->replay is nullptr.
 */
static int
ai_replay (struct ai_sink const *sink,
           struct cstr_view      json)
{
	return sink->replay ? sink->replay(sink->user, json) : 0;
}

/* Pass the text of @p val, escaped as the inside of a JSON string, to
 * sink->replay, unless sink->replay is nullptr or @p val is not a string.
 */
static int
ai_replay_text (struct ai_sink const *sink,
                struct json_object   *val)
{
	if (!sink->replay || !json_object_is_type(val, json_type_string))
		return 0;
	struct cstr_view json;
	int e = ai_json_text(val, &json);
	return e ? e : sink->replay(sink->user, cstr_view_slice(json, 1, json.len - 2));
}

/* Pass @p head and then the text of @p val, escaped as the inside of a
 * JSON string, to sink->replay.
 */
static int
ai_replay_open (struct ai_sink const *sink,
                struct cstr_view      head,
                struct json_object   *val)
{
	int e = ai_replay(sink, head);
	return e ? e : ai_replay_text(sink, val);
}

/* Pass @p head and then @p val, an item of the answer, whole, to
 * sink->replay, unless sink->replay is nullptr.
 */
static int
ai_replay_after (struct ai_sink const *sink,
                 struct cstr_view      head,
                 struct json_object   *val)
{
	if (!sink->replay)
		return 0;
	struct cstr_view json;
	int e = ai_json_text(val, &json);
	e = e ? e : sink->replay(sink->user, head);
	return e ? e : sink->replay(sink->user, json);
}

/* Pass a comma and then @p val, an item of the answer, whole, to
 * sink->replay.
 */
static int
ai_replay_item (struct ai_sink const *sink,
                struct json_object   *val)
{
	return ai_replay_after(sink, CSTR_VIEW(","), val);
}

/* Pass the parts of the tool calls of chat completions, @p calls, to
 * sink->call.
 */
static int
ai_chat_calls (struct ai_sink const *sink,
               struct json_object   *calls)
{
	if (!sink->call)
		return 0;
	int e = 0;
	struct json_object *c;
	for (size_t i = 0; !e && (c = jb_at(calls, i)); ++i) {
		struct json_object *fn = jb_get(c, "function");
		struct ai_call call = {
			.id    = jb_view(jb_get(c, "id")),
			.name  = jb_view(jb_get(fn, "name")),
			.args  = jb_view(jb_get(fn, "arguments")),
			.index = ai_index(jb_get(c, "index")),
		};
		/* extra_content contains the thought signature of Gemini, which
		 * askai sends back unchanged in the next turn. */
		e = ai_json_text(jb_get(c, "extra_content"), &call.extra);
		e = e ? e : sink->call(sink->user, &call);
	}
	return e;
}

/* Return true if @p val is a string that is not empty. */
static bool
ai_some_text (struct json_object *val)
{
	return jb_view(val).len;
}

/* The message of an error event, or of one that carries an error. */
static char const *
ai_event_error (struct json_object *ev)
{
	struct json_object *type = jb_get(ev, "type");
	struct json_object *error = jb_get(ev, "error");
	if (!error && jb_is(type, "response.failed"))
		error = jb_get(jb_get(ev, "response"), "error");
	/* An event of Gemini's Interactions has its type in event_type. */
	bool erred = error || jb_is(type, "error") ||
	             jb_is(jb_get(ev, "event_type"), "error");
	struct json_object *const words[] = {
		jb_get(error, "message"), error, erred ? jb_get(ev, "message") : nullptr
	};
	for (size_t i = 0; i < ARRAY_SIZE(words); ++i) {
		char const *msg = jb_text(words[i]);
		if (msg)
			return msg;
	}
	return erred ? "an error event" : nullptr;
}

bool
ai_cut (enum ai_api       api,
        char const       *reason)
{
	return reason && !strcmp(reason, limits[api]);
}

/* Pass the pieces of @p delta of chat completions to @p sink. Set
 * out->thought if the delta contains thinking.
 */
static int
ai_chat_delta (struct ai_sink const *sink,
               struct json_object   *delta,
               struct ai_event      *out)
{
	/* llama-server and DeepSeek use reasoning_content, and OpenRouter uses
	 * the alias reasoning. Thinking precedes visible text when a provider
	 * includes both in one delta. */
	struct json_object *thinking = jb_get(delta, "reasoning_content");
	if (!ai_some_text(thinking))
		thinking = jb_get(delta, "reasoning");
	int e = ai_put_thought(sink, jb_view(thinking), out);
	if (e)
		return e;
	struct json_object *content = jb_get(delta, "content");
	/* Some servers send content as an array of text parts. */
	e = ai_put_text(sink, jb_view(content));
	struct json_object *part;
	for (size_t i = 0; !e && (part = jb_at(content, i)); ++i)
		e = ai_put_text(sink, jb_view(jb_get(part, "text")));
	e = e ? e : ai_put_text(sink, jb_view(jb_get(delta, "refusal")));
	return e ? e : ai_chat_calls(sink, jb_get(delta, "tool_calls"));
}

/* Pass @p item, an output item of the Responses API at the place @p at in
 * the output, to @p sink. A function call goes to sink->call. A function
 * call, a reasoning item or a message goes whole to the replay. Set
 * out->thought_end if @p item is a reasoning item.
 */
static int
ai_responses_item (struct ai_sink const *sink,
                   struct json_object   *at,
                   struct json_object   *item,
                   struct ai_event      *out)
{
	enum { CALL, REASONING, MESSAGE, KINDS };
	static char const *const kinds[KINDS] = {
		[CALL]      = "function_call",
		[REASONING] = "reasoning",
		[MESSAGE]   = "message",
	};
	size_t kind = ai_kind(jb_get(item, "type"), kinds, KINDS);
	if (kind == KINDS)
		return 0;
	out->thought_end = kind == REASONING;
	int e = 0;
	if (kind == CALL && sink->call) {
		struct ai_call call = {
			.id    = jb_view(jb_get(item, "call_id")),
			.name  = jb_view(jb_get(item, "name")),
			.args  = jb_view(jb_get(item, "arguments")),
			.index = ai_index(at),
		};
		e = sink->call(sink->user, &call);
	}
	return e ? e : ai_replay_item(sink, item);
}

/* Pass the pieces of the event @p ev of the Responses API to @p sink. Set
 * out->thought if the event contains thinking. Set out->end and out->used
 * when the answer ends.
 */
static int
ai_responses_event (struct ai_sink const *sink,
                    struct json_object   *ev,
                    struct ai_event      *out)
{
	enum { TEXT, REFUSAL, REASONING, SUMMARY, ITEM, DONE, CUT, KINDS };
	static char const *const kinds[KINDS] = {
		[TEXT]      = "response.output_text.delta",
		[REFUSAL]   = "response.refusal.delta",
		[REASONING] = "response.reasoning_text.delta",
		[SUMMARY]   = "response.reasoning_summary_text.delta",
		[ITEM]      = "response.output_item.done",
		[DONE]      = "response.completed",
		[CUT]       = "response.incomplete",
	};
	struct json_object *delta = jb_get(ev, "delta");
	switch (ai_kind(jb_get(ev, "type"), kinds, KINDS)) {
	case TEXT:
	case REFUSAL:
		return ai_put_text(sink, jb_view(delta));
	case REASONING:
	case SUMMARY:
		return ai_put_thought(sink, jb_view(delta), out);
	case ITEM:
		return ai_responses_item(sink, jb_get(ev, "output_index"),
		                         jb_get(ev, "item"), out);
	case DONE:
	case CUT: {
		struct json_object *usage = jb_get(jb_get(ev, "response"), "usage");
		out->end = 1;
		out->used = ai_used(ai_count(jb_get(usage, "input_tokens")),
		                    ai_count(jb_get(usage, "output_tokens")));
		return 0;
	}
	}
	return 0;
}

/* Pass the start of tool call @p at, @p item, to sink->call. The parts are
 * the id and the name of @p item, the arguments in the member @p key of
 * @p item if they are a JSON object that is not empty, and the JSON text
 * of @p extra if @p extra is not nullptr. A server that sends the
 * arguments whole sends them in the start.
 */
static int
ai_call_start (struct ai_sink const *sink,
               uint32_t              at,
               struct json_object   *item,
               char const           *key,
               struct json_object   *extra)
{
	if (!sink->call)
		return 0;
	struct json_object *args = jb_get(item, key);
	bool whole = json_object_is_type(args, json_type_object) &&
	             json_object_object_length(args);
	struct ai_call call = {
		.id    = jb_view(jb_get(item, "id")),
		.name  = jb_view(jb_get(item, "name")),
		.index = at,
	};
	int e = ai_json_text(whole ? args : nullptr, &call.args);
	e = e ? e : ai_json_text(extra, &call.extra);
	return e ? e : sink->call(sink->user, &call);
}

/* Pass the start of @p block, the content block @p at of Anthropic's
 * Messages, to @p sink. The id and the name of a tool call go to
 * sink->call. The start of a thinking block goes to the replay, and
 * reader->open and reader->calls are set to that block. A redacted
 * thinking block goes whole to the replay.
 */
static int
ai_messages_start (struct ai_sink const *sink,
                   struct ai_reader     *reader,
                   uint32_t              at,
                   struct json_object   *block)
{
	enum { TOOL, THINKING, REDACTED, KINDS };
	static char const *const kinds[KINDS] = {
		[TOOL]     = "tool_use",
		[THINKING] = "thinking",
		[REDACTED] = "redacted_thinking",
	};
	switch (ai_kind(jb_get(block, "type"), kinds, KINDS)) {
	case TOOL:
		return ai_call_start(sink, at, block, "input", nullptr);
	case THINKING:
		reader->open = at + 1;
		reader->calls = at + 1;
		return ai_replay(sink, CSTR_VIEW(",{\"type\":\"thinking\",\"thinking\":\""));
	case REDACTED:
		return ai_replay_item(sink, block);
	}
	return 0;
}

/* Pass the pieces of @p delta, a delta of the content block @p at of
 * Anthropic's Messages, to @p sink. Text goes to sink->text, thinking to
 * sink->thought, and a piece of the arguments of a tool call to
 * sink->call. If the item of block @p at is open in the replay
 * (reader->open), the text, the thinking and the signature also go to the
 * replay. Otherwise the first text of a text block that is not only white
 * space opens the item of the block, because the API refuses a blank text
 * block in a turn. Set out->thought if the delta contains thinking.
 */
static int
ai_messages_delta (struct ai_sink const *sink,
                   struct ai_reader     *reader,
                   uint32_t              at,
                   struct json_object   *delta,
                   struct ai_event      *out)
{
	enum { TEXT, THINKING, SIGNATURE, INPUT, KINDS };
	static char const *const kinds[KINDS] = {
		[TEXT]      = "text_delta",
		[THINKING]  = "thinking_delta",
		[SIGNATURE] = "signature_delta",
		[INPUT]     = "input_json_delta",
	};
	bool open = reader->open == at + 1;
	switch (ai_kind(jb_get(delta, "type"), kinds, KINDS)) {
	case TEXT: {
		struct json_object *text = jb_get(delta, "text");
		struct cstr_view piece = jb_view(text);
		int e = ai_put_text(sink, piece);
		if (e)
			return e;
		if (open)
			return ai_replay_text(sink, text);
		if (!cstr_view_trim_end(piece).len)
			return 0;
		reader->open = at + 1;
		return ai_replay_open(sink, CSTR_VIEW(",{\"type\":\"text\",\"text\":\""), text);
	}
	case THINKING: {
		struct json_object *text = jb_get(delta, "thinking");
		int e = ai_put_thought(sink, jb_view(text), out);
		return e || !open ? e : ai_replay_text(sink, text);
	}
	case SIGNATURE:
		return open ? ai_replay_open(sink, CSTR_VIEW("\",\"signature\":\""),
		                             jb_get(delta, "signature"))
		            : 0;
	case INPUT: {
		if (!sink->call)
			return 0;
		struct ai_call call = {
			.args  = jb_view(jb_get(delta, "partial_json")),
			.index = at,
		};
		return sink->call(sink->user, &call);
	}
	}
	return 0;
}

/* Pass the pieces of the event @p ev of Anthropic's Messages to @p sink,
 * with the state of the reading in @p reader. Set out->thought if the
 * event contains thinking, out->used when the answer ends, and out->end at
 * the end of the message. An event of a content block without an index
 * can pass only text to @p sink.
 */
static int
ai_messages_event (struct ai_sink const *sink,
                   struct ai_reader     *reader,
                   struct json_object   *ev,
                   struct ai_event      *out)
{
	/* The content blocks' events first. */
	enum { DELTA, START, STOP, MESSAGE, MESSAGE_DELTA, MESSAGE_STOP, KINDS };
	static char const *const kinds[KINDS] = {
		[DELTA]         = "content_block_delta",
		[START]         = "content_block_start",
		[STOP]          = "content_block_stop",
		[MESSAGE]       = "message_start",
		[MESSAGE_DELTA] = "message_delta",
		[MESSAGE_STOP]  = "message_stop",
	};
	uint32_t at = ai_index(jb_get(ev, "index"));
	size_t kind = ai_kind(jb_get(ev, "type"), kinds, KINDS);
	struct json_object *delta = jb_get(ev, "delta");
	if (kind < MESSAGE && at == AI_CALL_NEXT)
		return kind == DELTA ? ai_put_text(sink, jb_view(jb_get(delta, "text"))) : 0;
	switch (kind) {
	case DELTA:
		return ai_messages_delta(sink, reader, at, delta, out);
	case START:
		return ai_messages_start(sink, reader, at, jb_get(ev, "content_block"));
	case STOP:
		if (reader->open != at + 1)
			return 0;
		out->thought_end = reader->calls == at + 1;
		reader->calls = 0;
		reader->open = 0;
		return ai_replay(sink, CSTR_VIEW("\"}"));
	case MESSAGE: {
		struct json_object *usage = jb_get(jb_get(ev, "message"), "usage");
		struct json_object *written = jb_get(usage, "cache_creation_input_tokens");
		struct json_object *cached = jb_get(usage, "cache_read_input_tokens");
		int64_t n = ai_count(jb_get(usage, "input_tokens"));
		/* A sum that overflows int64_t is not a token count. */
		bool over = ckd_add(&n, n, ai_count(written)) ||
		            ckd_add(&n, n, ai_count(cached));
		reader->input = over ? 0 : ai_used(n, 0);
		return 0;
	}
	case MESSAGE_DELTA: {
		struct json_object *usage = jb_get(ev, "usage");
		out->used = ai_used(reader->input,
		                    ai_count(jb_get(usage, "output_tokens")));
		return 0;
	}
	case MESSAGE_STOP:
		out->end = 1;
		return 0;
	}
	return 0;
}

/* Close the text part that is open in the replay of @p sink, if
 * reader->open is set.
 */
static int
ai_generate_close (struct ai_sink const *sink,
                   struct ai_reader     *reader)
{
	if (!reader->open)
		return 0;
	reader->open = 0;
	return ai_replay(sink, CSTR_VIEW("\"}"));
}

/* Pass the function call @p fc of Gemini to sink->call with the index
 * reader->calls, and increment reader->calls.
 */
static int
ai_generate_call (struct ai_sink const *sink,
                  struct ai_reader     *reader,
                  struct json_object   *fc)
{
	if (!sink->call)
		return 0;
	struct ai_call call = {
		.id    = jb_view(jb_get(fc, "id")),
		.name  = jb_view(jb_get(fc, "name")),
		.index = reader->calls++,
	};
	int e = ai_json_text(jb_get(fc, "args"), &call.args);
	return e ? e : sink->call(sink->user, &call);
}

/* Close the text part that is open in the replay, if there is one, and then
 * pass @p part whole to the replay.
 */
static int
ai_generate_whole (struct ai_sink const *sink,
                   struct ai_reader     *reader,
                   struct json_object   *part)
{
	int e = ai_generate_close(sink, reader);
	return e ? e : ai_replay_item(sink, part);
}

/* Pass @p part, a part of the answer of Gemini, to @p sink, with the state
 * of the reading in @p reader. A function call goes to sink->call and
 * whole to the replay. A part with a thought signature also goes whole to
 * the replay. Thinking goes to sink->thought and sets out->thought. Other
 * text goes to sink->text and into the text part that is open in the
 * replay, or into a new text part if none is open.
 */
static int
ai_generate_part (struct ai_sink const *sink,
                  struct ai_reader     *reader,
                  struct json_object   *part,
                  struct ai_event      *out)
{
	struct json_object *fc = jb_get(part, "functionCall");
	if (fc) {
		int e = ai_generate_close(sink, reader);
		e = e ? e : ai_generate_call(sink, reader, fc);
		return e ? e : ai_replay_item(sink, part);
	}
	struct json_object *text = jb_get(part, "text");
	struct cstr_view piece = jb_view(text);
	struct json_object *signature = jb_get(part, "thoughtSignature");
	if (json_object_get_boolean(jb_get(part, "thought"))) {
		int e = ai_put_thought(sink, piece, out);
		return e ? e : signature ? ai_generate_whole(sink, reader, part) : 0;
	}
	int e = ai_put_text(sink, piece);
	if (e)
		return e;
	if (signature)
		return ai_generate_whole(sink, reader, part);
	if (!piece.len)
		return 0;
	if (reader->open)
		return ai_replay_text(sink, text);
	reader->open = 1;
	return ai_replay_open(sink, CSTR_VIEW(",{\"text\":\""), text);
}

/* Return true if the open thought of @p reader is in the replay, which is
 * the case once a summary item or a signature of the thought is written.
 * The API refuses a thought with neither.
 */
static bool
ai_interactions_written (struct ai_reader const *reader)
{
	return reader->summary != SUMMARY_NONE || reader->sign;
}

/* Write the start of the open thought of @p reader into the replay of
 * @p sink, unless the start is there already.
 */
static int
ai_interactions_head (struct ai_sink const   *sink,
                      struct ai_reader const *reader)
{
	if (ai_interactions_written(reader))
		return 0;
	return ai_replay(sink, CSTR_VIEW(",{\"type\":\"thought\""));
}

/* Close the item that is open in the replay of @p sink, if reader->open
 * is set. For a thought, close its summary array if it is open, and then
 * the thought if it is in the replay. For a model output, close its text
 * and the step.
 */
static int
ai_interactions_close (struct ai_sink const *sink,
                       struct ai_reader     *reader)
{
	if (!reader->open)
		return 0;
	reader->open = 0;
	if (reader->kind == INTERACTIONS_OUTPUT)
		return ai_replay(sink, CSTR_VIEW("\"}]}"));
	if (!ai_interactions_written(reader))
		return 0;
	return reader->summary == SUMMARY_OPEN ? ai_replay(sink, CSTR_VIEW("]}"))
	                                       : ai_replay(sink, CSTR_VIEW("}"));
}

/* Pass @p text, a piece of text of step @p at of Gemini's Interactions, to
 * sink->text and to the replay. The text goes into the open item if that
 * item is the model output of step @p at. If no item is open, text that is
 * not only white space starts a new model output item for step @p at.
 * Otherwise the text does not go to the replay.
 */
static int
ai_interactions_text (struct ai_sink const *sink,
                      struct ai_reader     *reader,
                      uint32_t              at,
                      struct json_object   *text)
{
	struct cstr_view piece = jb_view(text);
	if (!piece.data)
		return 0;
	int e = sink->text(sink->user, piece);
	if (e)
		return e;
	if (reader->open == at + 1)
		return reader->kind == INTERACTIONS_OUTPUT ? ai_replay_text(sink, text) : 0;
	if (reader->open || !cstr_view_trim_end(piece).len)
		return 0;
	reader->open = at + 1;
	reader->kind = INTERACTIONS_OUTPUT;
	return ai_replay_open(sink, CSTR_VIEW(",{\"type\":\"model_output\",\"content\":"
	                                      "[{\"type\":\"text\",\"text\":\""), text);
}

/* Write @p item, a summary item of the open thought, whole to the replay
 * of @p sink. The item goes after the earlier items, or into a new
 * summary array after the start of the thought if the thought is not in
 * the replay yet. Skip an item that is not a JSON object, and an item that
 * comes after the array is closed.
 */
static int
ai_interactions_summary (struct ai_sink const *sink,
                         struct ai_reader     *reader,
                         struct json_object   *item)
{
	if (reader->summary == SUMMARY_CLOSED || !json_object_is_type(item, json_type_object))
		return 0;
	if (reader->summary == SUMMARY_OPEN)
		return ai_replay_item(sink, item);
	int e = ai_interactions_head(sink, reader);
	reader->summary = SUMMARY_OPEN;
	return e ? e : ai_replay_after(sink, CSTR_VIEW(",\"summary\":["), item);
}

/* Write @p signature into the open thought in the replay of @p sink, after
 * the start of the thought if the thought is not in the replay yet. Close
 * the summary array first if it is open. Skip a signature that is not a
 * string, that is empty, or that comes after a signature of the thought.
 */
static int
ai_interactions_sign (struct ai_sink const *sink,
                      struct ai_reader     *reader,
                      struct json_object   *signature)
{
	if (reader->sign || !ai_some_text(signature))
		return 0;
	int e = ai_interactions_head(sink, reader);
	reader->sign = 1;
	if (!e && reader->summary == SUMMARY_OPEN) {
		reader->summary = SUMMARY_CLOSED;
		e = ai_replay(sink, CSTR_VIEW("]"));
	}
	e = e ? e : ai_replay_open(sink, CSTR_VIEW(",\"signature\":\""), signature);
	return e ? e : ai_replay(sink, CSTR_VIEW("\""));
}

/* Mark @p step, the thought step @p at of Gemini's Interactions, open in
 * @p reader, and write the signature and the summary items in the start of
 * the step into the replay of @p sink. The thought is written to the
 * replay with its first summary item or its signature. Set out->thought if
 * the text of a summary item is not empty.
 */
static int
ai_interactions_thought (struct ai_sink const *sink,
                         struct ai_reader     *reader,
                         uint32_t              at,
                         struct json_object   *step,
                         struct ai_event      *out)
{
	reader->open = at + 1;
	reader->kind = INTERACTIONS_THOUGHT;
	reader->summary = SUMMARY_NONE;
	reader->sign = 0;
	int e = ai_interactions_sign(sink, reader, jb_get(step, "signature"));
	struct json_object *summary = jb_get(step, "summary");
	struct json_object *item;
	for (size_t i = 0; !e && (item = jb_at(summary, i)); ++i) {
		e = ai_put_thought(sink, jb_view(jb_get(item, "text")), out);
		e = e ? e : ai_interactions_summary(sink, reader, item);
	}
	return e;
}

/* Process @p step, the start of step @p at of Gemini's Interactions.
 * Ignore a step of a type other than thought, model_output and
 * function_call, such as the step of a server tool; the open item stays
 * open. For those three types, close the open item in the replay first.
 * Then open a thought (ai_interactions_thought()), pass the text of a model
 * output (ai_interactions_text()), or mark the function call as the last
 * that started in @p reader and pass it to @p sink.
 */
static int
ai_interactions_start (struct ai_sink const *sink,
                       struct ai_reader     *reader,
                       uint32_t              at,
                       struct json_object   *step,
                       struct ai_event      *out)
{
	enum { THOUGHT, OUTPUT, CALL, KINDS };
	static char const *const kinds[KINDS] = {
		[THOUGHT] = "thought",
		[OUTPUT]  = "model_output",
		[CALL]    = "function_call",
	};
	size_t kind = ai_kind(jb_get(step, "type"), kinds, KINDS);
	if (kind == KINDS)
		return 0;
	int e = ai_interactions_close(sink, reader);
	if (e)
		return e;
	switch (kind) {
	case THOUGHT:
		return ai_interactions_thought(sink, reader, at, step, out);
	case OUTPUT: {
		struct json_object *content = jb_get(step, "content");
		struct json_object *item;
		for (size_t i = 0; !e && (item = jb_at(content, i)); ++i)
			if (jb_is(jb_get(item, "type"), "text"))
				e = ai_interactions_text(sink, reader, at,
				                         jb_get(item, "text"));
		return e;
	}
	case CALL: {
		/* The signature that the first call of a turn carries. */
		struct json_object *signature = jb_get(step, "signature");
		reader->calls = at + 1;
		return ai_call_start(sink, at, step, "arguments",
		                     ai_some_text(signature) ? signature : nullptr);
	}
	}
	return 0;
}

/* Process @p delta, a delta of step @p at of Gemini's Interactions. Pass
 * the text of a model output (ai_interactions_text()). Write a summary
 * item or a signature into the thought of step @p at if that thought is
 * open, and set out->thought if the text of the summary item is not
 * empty. Pass a piece of the arguments of a function call to sink->call.
 * If step @p at is not the function call that started last
 * (reader->calls), close the open item in the replay first, because a
 * piece that starts a new call places the call at the current end of the
 * replay, and mark step @p at as that call.
 */
static int
ai_interactions_delta (struct ai_sink const *sink,
                       struct ai_reader     *reader,
                       uint32_t              at,
                       struct json_object   *delta,
                       struct ai_event      *out)
{
	enum { TEXT, SUMMARY, SIGNATURE, ARGUMENTS, KINDS };
	static char const *const kinds[KINDS] = {
		[TEXT]      = "text",
		[SUMMARY]   = "thought_summary",
		[SIGNATURE] = "thought_signature",
		[ARGUMENTS] = "arguments_delta",
	};
	bool thought = reader->open == at + 1 && reader->kind == INTERACTIONS_THOUGHT;
	switch (ai_kind(jb_get(delta, "type"), kinds, KINDS)) {
	case TEXT:
		return ai_interactions_text(sink, reader, at, jb_get(delta, "text"));
	case SUMMARY: {
		struct json_object *item = jb_get(delta, "content");
		int e = ai_put_thought(sink, jb_view(jb_get(item, "text")), out);
		return e || !thought ? e : ai_interactions_summary(sink, reader, item);
	}
	case SIGNATURE:
		return thought ? ai_interactions_sign(sink, reader, jb_get(delta, "signature"))
		               : 0;
	case ARGUMENTS: {
		if (!sink->call)
			return 0;
		struct ai_call call = {
			.args  = jb_view(jb_get(delta, "arguments")),
			.index = at,
		};
		if (!call.args.data)
			return 0;
		int e = reader->calls == at + 1 ? 0 : ai_interactions_close(sink, reader);
		reader->calls = at + 1;
		return e ? e : sink->call(sink->user, &call);
	}
	}
	return 0;
}

/* Process @p ev, an event of Gemini's Interactions of type @p type, with
 * the state of the reading in @p reader. interaction.completed closes the
 * open item in the replay, sets out->end and the tokens used, and makes
 * the reading ignore the events after it. A step event without an index
 * can pass only text to @p sink.
 */
static int
ai_interactions_event (struct ai_sink const *sink,
                       struct ai_reader     *reader,
                       struct json_object   *type,
                       struct json_object   *ev,
                       struct ai_event      *out)
{
	enum { DELTA, START, STOP, DONE, KINDS };
	static char const *const kinds[KINDS] = {
		[DELTA] = "step.delta",
		[START] = "step.start",
		[STOP]  = "step.stop",
		[DONE]  = "interaction.completed",
	};
	if (reader->kind == INTERACTIONS_ENDED)
		return 0;
	uint32_t at = ai_index(jb_get(ev, "index"));
	size_t kind = ai_kind(type, kinds, KINDS);
	struct json_object *delta = jb_get(ev, "delta");
	if (kind < DONE && at == AI_CALL_NEXT)
		return kind == DELTA ? ai_put_text(sink, jb_view(jb_get(delta, "text"))) : 0;
	switch (kind) {
	case DELTA:
		return ai_interactions_delta(sink, reader, at, delta, out);
	case START:
		return ai_interactions_start(sink, reader, at, jb_get(ev, "step"), out);
	case STOP:
		if (reader->open != at + 1)
			return 0;
		out->thought_end = reader->kind == INTERACTIONS_THOUGHT;
		return ai_interactions_close(sink, reader);
	case DONE: {
		/* The thinking is not sent back in the next request, so the tokens
		 * used are total_input_tokens plus total_output_tokens, which leave
		 * out the thinking. If that sum is not known, the tokens used are
		 * total_tokens, which include the thinking. */
		struct json_object *usage = jb_get(jb_get(ev, "interaction"), "usage");
		out->end = 1;
		out->used = ai_used(ai_count(jb_get(usage, "total_input_tokens")),
		                    ai_count(jb_get(usage, "total_output_tokens")));
		if (!out->used)
			out->used = ai_used(ai_count(jb_get(usage, "total_tokens")), 0);
		int e = ai_interactions_close(sink, reader);
		reader->kind = INTERACTIONS_ENDED;
		return e;
	}
	}
	return 0;
}

int
ai_event (enum ai_api            api,
          struct ai_reader      *reader,
          struct json_object    *ev,
          struct ai_sink const  *sink,
          struct ai_event       *out)
{
	if (!reader || !sink || !sink->text || !out)
		return EINVAL;
	if ((out->error = ai_event_error(ev)))
		return 0;
	int e = 0;
	struct json_object *reason = nullptr;
	switch (api) {
	case AI_CHAT: {
		/* The first completion's, should --extra ask for more (n). */
		struct json_object *choices = jb_get(ev, "choices");
		struct json_object *choice;
		for (size_t i = 0; (choice = jb_at(choices, i)) &&
		                   ai_count(jb_get(choice, "index")); ++i)
			;
		/* With include_usage, a last event of its own, with no choices. */
		struct json_object *usage = jb_get(ev, "usage");
		out->used = ai_used(ai_count(jb_get(usage, "prompt_tokens")),
		                    ai_count(jb_get(usage, "completion_tokens")));
		reason = jb_get(choice, "finish_reason");
		e = ai_chat_delta(sink, jb_get(choice, "delta"), out);
		break;
	}
	case AI_RESPONSES:
		e = ai_responses_event(sink, ev, out);
		reason = jb_get(jb_get(jb_get(ev, "response"), "incomplete_details"),
		                "reason");
		break;
	case AI_MESSAGES:
		e = ai_messages_event(sink, reader, ev, out);
		reason = jb_get(jb_get(ev, "delta"), "stop_reason");
		break;
	case AI_GENERATE: {
		struct json_object *candidate = jb_at(jb_get(ev, "candidates"), 0);
		struct json_object *parts = jb_get(jb_get(candidate, "content"), "parts");
		struct json_object *part;
		for (size_t i = 0; !e && (part = jb_at(parts, i)); ++i)
			e = ai_generate_part(sink, reader, part, out);
		reason = candidate ? jb_get(candidate, "finishReason")
		       : jb_get(jb_get(ev, "promptFeedback"), "blockReason");
		if (!e && reason)
			e = ai_generate_close(sink, reader);
		struct json_object *usage = jb_get(ev, "usageMetadata");
		out->used = ai_used(ai_count(jb_get(usage, "totalTokenCount")), 0);
		break;
	}
	case AI_INTERACTIONS:
		/* interaction.created has a status too, so only the status in
		 * interaction.completed is the stop reason. */
		e = ai_interactions_event(sink, reader, jb_get(ev, "event_type"), ev, out);
		reason = out->end ? jb_get(jb_get(ev, "interaction"), "status") : nullptr;
		break;
	}
	out->reason = jb_text(reason);
	out->truncated = ai_cut(api, out->reason);
	out->end = out->end || out->reason;
	return e;
}

struct ai_format const *
ai_format (enum ai_api api)
{
#define B(s) CSTR_VIEW_INIT(s)
	static struct ai_format const formats[] = {
		[AI_CHAT] = {
			.tools        = B("["),
			.tools_end    = B("]"),
			.turn         = B(",{\"role\":\"assistant\""),
			.text         = B(",\"content\":\""),
			.thought      = B("\",\"reasoning_content\":\""),
			.calls        = B("\",\"tool_calls\":["),
			.call         = B("{\"id\":\""),
			.call_next    = B(",{\"id\":\""),
			.name         = B("\",\"type\":\"function\",\"function\":{\"name\":\""),
			.args         = B("\",\"arguments\":\""),
			.args_end     = B("\"}"),
			.extra        = B(",\"extra_content\":"),
			.call_end     = B("}"),
			.turn_end     = B("]}"),
			.answer_end   = B("\"}"),
			.result       = B(",{\"role\":\"tool\",\"tool_call_id\":\""),
			.output       = B("\",\"content\":\""),
			.result_end   = B("\"}"),
			.user         = B(",{\"role\":\"user\",\"content\":\""),
			.prompt_end   = B("\"}"),
			.end          = B("]}"),
			.conversation = "messages",
		},
		/* Its turn is the replay's items, its calls among them, in the
		 * input's list. */
		[AI_RESPONSES] = {
			.tools        = B("["),
			.tools_end    = B("]"),
			.result       = B(",{\"type\":\"function_call_output\",\"call_id\":\""),
			.output       = B("\",\"output\":\""),
			.result_end   = B("\"}"),
			.user         = B(",{\"role\":\"user\",\"content\":\""),
			.prompt_end   = B("\"}"),
			.end          = B("]}"),
			.conversation = "input",
		},
		/* Its turn's text and thinking blocks are in the replay; all its
		 * results go in one message. */
		[AI_MESSAGES] = {
			.tools        = B("["),
			.tools_end    = B("]"),
			.turn         = B(",{\"role\":\"assistant\",\"content\":["),
			.call         = B("{\"type\":\"tool_use\",\"id\":\""),
			.call_next    = B(",{\"type\":\"tool_use\",\"id\":\""),
			.name         = B("\",\"name\":\""),
			.args         = B("\",\"input\":"),
			.call_end     = B("}"),
			.turn_end     = B("]}"),
			.answer_end   = B("]}"),
			.results      = B(",{\"role\":\"user\",\"content\":["),
			.result_sep   = B(","),
			.result       = B("{\"type\":\"tool_result\",\"tool_use_id\":\""),
			.output       = B("\",\"content\":\""),
			.result_end   = B("\"}"),
			.results_end  = B("]}"),
			.user         = B(",{\"role\":\"user\",\"content\":\""),
			.prompt_end   = B("\"}"),
			.end          = B("]}"),
			.conversation = "messages",
			.flags        = AI_ARGS_RAW,
		},
		/* Its turn's parts are the replay's, its calls among them, with
		 * their thought signatures. A file goes after the results, in
		 * their message, as the model does not see a text file in a
		 * result. */
		[AI_GENERATE] = {
			.tools        = B("[{\"functionDeclarations\":["),
			.tools_end    = B("]}]"),
			.turn         = B(",{\"role\":\"model\",\"parts\":["),
			.turn_end     = B("]}"),
			.answer_end   = B("]}"),
			.results      = B(",{\"role\":\"user\",\"parts\":["),
			.result_sep   = B(","),
			.result       = B("{\"functionResponse\":{\"name\":\""),
			.result_id    = B("\",\"id\":\""),
			.output       = B("\",\"response\":{\"output\":\""),
			.result_end   = B("\"}}}"),
			.results_end  = B("]}"),
			.file         = B(",{\"fileData\":{\"mimeType\":\"text/plain\","
			                  "\"fileUri\":\""),
			.file_end     = B("\"}}"),
			.user         = B(",{\"role\":\"user\",\"parts\":[{\"text\":\""),
			.prompt_end   = B("\"}]}"),
			.end          = B("]}"),
			.conversation = "contents",
			.flags        = AI_RESULT_NAMED,
		},
		/* A turn is the replay's steps and the calls, in the input list,
		 * a call with its signature. Each result is a step after them,
		 * which the API refuses without the call's name. A file is a
		 * user_input step after the results, as a result takes no
		 * document. */
		[AI_INTERACTIONS] = {
			.tools        = B("["),
			.tools_end    = B("]"),
			.call         = B(",{\"type\":\"function_call\",\"id\":\""),
			.call_next    = B(",{\"type\":\"function_call\",\"id\":\""),
			.name         = B("\",\"name\":\""),
			.args         = B("\",\"arguments\":"),
			.extra        = B(",\"signature\":"),
			.call_end     = B("}"),
			.result       = B(",{\"type\":\"function_result\",\"name\":\""),
			.result_id    = B("\",\"call_id\":\""),
			.output       = B("\",\"result\":\""),
			.result_end   = B("\"}"),
			.file         = B(",{\"type\":\"user_input\",\"content\":"
			                  "[{\"type\":\"document\",\"mime_type\":"
			                  "\"text/plain\",\"uri\":\""),
			.file_end     = B("\"}]}"),
			.user         = B(",{\"type\":\"user_input\",\"content\":"
			                  "[{\"type\":\"text\",\"text\":\""),
			.prompt_end   = B("\"}]}"),
			.end          = B("]}"),
			.conversation = "input",
			.flags        = AI_ARGS_RAW | AI_RESULT_NAMED,
		},
	};
#undef B
	return &formats[api];
}

struct json_object *
ai_tool (enum ai_api               api,
         struct json_object const *def)
{
	static char const *const schemas[] = {
		[AI_CHAT]         = "parameters",
		[AI_RESPONSES]    = "parameters",
		[AI_MESSAGES]     = "input_schema",
		[AI_GENERATE]     = "parametersJsonSchema",
		[AI_INTERACTIONS] = "parameters",
	};
	struct json_object *tool = jb_object(
		"name", json_object_get(jb_get(def, "name")),
		"description", json_object_get(jb_get(def, "description")),
		schemas[api], json_object_get(jb_get(def, "inputSchema")),
		nullptr);
	switch (api) {
	case AI_CHAT:
		return jb_object("type", json_object_new_string("function"),
		                 "function", tool, nullptr);
	case AI_RESPONSES:
	case AI_INTERACTIONS:
		return jb_set_or_free(tool, "type", json_object_new_string("function"));
	case AI_MESSAGES:
	case AI_GENERATE:
		break;
	}
	return tool;
}

/* The data of the first event of the event stream at @p body, of @p len
 * bytes, parsed, if it is JSON text on a data line that ends in the first
 * 4 KiB; else nullptr. Gemini's Interactions sends the error of a streamed
 * request as such an event, with a status that is not 2xx, so askai gathers
 * the body whole and cannot parse it as JSON text.
 */
static struct json_object *
ai_error_event (char const *body,
                size_t      len)
{
	static char const field[] = "data:";
	size_t n = len < 4096 ? len : 4096;
	for (size_t at = 0, end; at < n; at = end + 1) {
		char const *nl = memchr(body + at, '\n', n - at);
		if (!nl)
			break;
		end = (size_t)(nl - body);
		if (end - at < sizeof field - 1 || memcmp(body + at, field, sizeof field - 1))
			continue;
		size_t from = at + sizeof field - 1;
		end -= end > from && body[end - 1] == '\r';
		return jb_parse(body + from, end - from, JSON_TOKENER_STRICT, 8);
	}
	return nullptr;
}

/* Say in @p text what the upstream's error answer, of HTTP @p status, says:
 * its message, or that of its first event, or the start of the @p len bytes
 * at @p body.
 */
static int
ai_describe_error (struct dstr        *text,
                   long                status,
                   struct json_object *root,
                   char const         *body,
                   size_t              len)
{
	struct json_object *error = jb_get(root, "error");
	struct json_object *const words[] = {
		jb_get(error, "message"), error, jb_get(root, "message"),
		jb_get(root, "detail")
	};
	if (!dstr_printf(text, "Upstream error (HTTP %ld): ", status))
		return ENOMEM;
	for (size_t i = 0; i < ARRAY_SIZE(words); ++i) {
		char const *msg = jb_text(words[i]);
		if (msg)
			return dstr_cat(text, msg) ? 0 : ENOMEM;
	}
	struct json_object *event = root ? nullptr : ai_error_event(body, len);
	char const *said = ai_event_error(event);
	if (said) {
		bool ok = dstr_cat(text, said);
		json_object_put(event);
		return ok ? 0 : ENOMEM;
	}
	json_object_put(event);

	/* Quote the start of an unparsable body, as far as it is UTF-8 text. */
	size_t cut = jb_utf8(body, len < 512 ? len : 512);
#define QUOTE(s) dstr_cat_n(text, s, sizeof s - 1)
	bool ok = cut ? dstr_cat_n(text, body, cut)
	        : len ? QUOTE("a response that is not text")
	              : QUOTE("empty response");
#undef QUOTE
	return ok ? 0 : ENOMEM;
}

struct ai_upload const *
ai_upload (enum ai_api api)
{
	/* Gemini's Files API, which keeps a file for 48 hours. It takes the
	 * file as the whole body with the protocol raw. */
	static char const *const gemini_lines[] = {
		"X-Goog-Upload-Protocol: raw", "Content-Type: text/plain", nullptr
	};
	static struct ai_upload const gemini = {
		.path = "/upload/v1beta/files",
		.lines = gemini_lines
	};
	return api == AI_GENERATE || api == AI_INTERACTIONS ? &gemini : nullptr;
}

char *
ai_gemini_url (char const *url,
               char const *path,
               char const *tail)
{
	static char const api[] = "/v1beta/";
	static char const key[] = "key=";
	char const *scheme = url && path ? strstr(url, "://") : nullptr;
	if (!scheme)
		return nullptr;
	char const *host = scheme + sizeof "://" - 1;
	size_t end = strcspn(host, "?#");
	char const *root = memmem(host, end, api, sizeof api - 1);
	if (!root)
		return nullptr;
	/* The last parameter key of the query, as the API reads it. */
	char const *query = host[end] == '?' ? host + end : nullptr;
	char const *found = nullptr;
	size_t len = 0;
	for (char const *p = query; p && (*p == '?' || *p == '&'); p += 1 + len) {
		len = strcspn(p + 1, "&#");
		if (len >= sizeof key - 1 && !strncmp(p + 1, key, sizeof key - 1))
			found = p + 1;
	}
	size_t key_len = found ? strcspn(found, "&#") : 0;
	tail = tail ? tail : "";
	size_t size = (size_t)(root - url) + strlen(path) + strlen(tail) + 1 + key_len + 1;
	char *out = malloc(size);
	int n = out ? snprintf(out, size, "%.*s%s%s%s%.*s", (int)(root - url), url, path,
	                       tail, found ? "?" : "", (int)key_len, found ? found : "")
	            : -1;
	if (n < 0 || (size_t)n >= size) {
		free(out);
		return nullptr;
	}
	return out;
}

char *
ai_upload_url (struct ai_endpoint const *ep)
{
	struct ai_upload const *up = ep ? ai_upload(ep->api) : nullptr;
	return up ? ai_gemini_url(ep->url, up->path, nullptr) : nullptr;
}

char *
ai_model_url (struct ai_endpoint const *ep)
{
	bool gemini = ep && ep->model &&
	              (ep->api == AI_GENERATE || ep->api == AI_INTERACTIONS);
	return gemini ? ai_gemini_url(ep->url, "/v1beta/models/", ep->model) : nullptr;
}

/* @p text, a time of RFC 3339 in UTC, such as 2026-10-17T13:44:05.888Z, in
 * seconds since the epoch, without its fraction; 0 if it is no such time.
 */
static int64_t
ai_time (char const *text)
{
	struct tm tm = {};
	int n = 0;
	if (!text || sscanf(text, "%4d-%2d-%2dT%2d:%2d:%2d%n", &tm.tm_year, &tm.tm_mon,
	                    &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec, &n) != 6)
		return 0;
	char const *zone = text + n;
	zone += *zone == '.' ? 1 + strspn(zone + 1, "0123456789") : 0;
	if (strcmp(zone, "Z"))
		return 0;
	tm.tm_year -= 1900;
	tm.tm_mon -= 1;
	time_t t = timegm(&tm);
	return t == (time_t)-1 ? 0 : (int64_t)t;
}

/* The bytes of a URI that askai takes from an upstream: printable ASCII
 * without a space, which goes into JSON and the log as it is.
 */
static char const ai_uri_bytes[] =
	"!\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`"
	"abcdefghijklmnopqrstuvwxyz{|}~";

int
ai_uploaded (enum ai_api  api,
             long         status,
             char const  *body,
             size_t       len,
             struct dstr *uri,
             int64_t     *expires,
             struct dstr *why)
{
	if (!uri || !expires || !why || (!body && len))
		return EINVAL;
	*expires = 0;
	if (!ai_upload(api))
		return dstr_cat(why, "The upstream takes no files.") ? EIO : ENOMEM;
	struct json_object *root = len ? jb_parse(body, len, JSON_TOKENER_STRICT, 8)
	                               : nullptr;
	struct json_object *file = jb_get(root, "file");
	struct cstr_view at = jb_view(jb_get(file, "uri"));
	int e = 0;
	if (status < 200 || status > 299 || !at.data) {
		e = ai_describe_error(why, status, root, body, len);
		e = e ? e : EIO;
	} else if (!at.len || at.len > strspn(at.data, ai_uri_bytes)) {
		e = dstr_cat(why, "The upstream named the file with a URI that is not "
		             "printable ASCII.") ? EIO : ENOMEM;
	} else {
		*expires = ai_time(jb_text(jb_get(file, "expirationTime")));
		e = dstr_cat_n(uri, at.data, at.len) ? 0 : ENOMEM;
	}
	json_object_put(root);
	return e;
}

int
ai_answer (enum ai_api         api,
           long                status,
           struct json_object *root,
           char const         *body,
           size_t              len,
           struct dstr        *text,
           bool               *failed)
{
	*failed = status < 200 || status > 299 || !root || jb_get(root, "error");
	if (*failed)
		return ai_describe_error(text, status, root, body, len);

	char const *reason = nullptr;
	int e = 0;
	switch (api) {
	case AI_CHAT:
		e = ai_chat_answer(root, text, &reason);
		break;
	case AI_RESPONSES:
		e = ai_responses_answer(root, text, &reason);
		break;
	case AI_MESSAGES:
		e = ai_messages_answer(root, text, &reason);
		break;
	case AI_GENERATE:
		e = ai_generate_answer(root, text, &reason);
		break;
	case AI_INTERACTIONS:
		e = ai_interactions_answer(root, text, &reason);
		break;
	}
	if (e)
		return e;
	if (!dstr_len(text)) {
		*failed = true;
		bool ok = reason ? dstr_printf(text, no_answer_reason, reason)
		                 : dstr_cat_n(text, no_answer, sizeof no_answer - 1);
		return ok ? 0 : ENOMEM;
	}
	if (ai_cut(api, reason))
		return dstr_cat_n(text, AI_CUT_NOTE, sizeof AI_CUT_NOTE - 1) ? 0 : ENOMEM;
	return 0;
}

/* Append the decoded text of @p parts, an array of text-bearing objects. */
static int
ai_thought_parts (struct dstr        *text,
                  struct json_object *parts)
{
	int e = 0;
	struct json_object *part;
	for (size_t i = 0; !e && (part = jb_at(parts, i)); ++i)
		e = ai_append(text, jb_get(part, "text"));
	return e;
}

int
ai_thoughts (enum ai_api         api,
             struct json_object *root,
             struct dstr        *text)
{
	struct json_object *items;
	switch (api) {
	case AI_CHAT: {
		struct json_object *choice = jb_at(jb_get(root, "choices"), 0);
		struct json_object *message = jb_get(choice, "message");
		struct json_object *thought = jb_get(message, "reasoning_content");
		if (!ai_some_text(thought))
			thought = jb_get(message, "reasoning");
		return ai_append(text, thought);
	}
	case AI_RESPONSES:
		items = jb_get(root, "output");
		break;
	case AI_MESSAGES:
		items = jb_get(root, "content");
		break;
	case AI_GENERATE: {
		struct json_object *candidate = jb_at(jb_get(root, "candidates"), 0);
		items = jb_get(jb_get(candidate, "content"), "parts");
		break;
	}
	case AI_INTERACTIONS:
		items = jb_get(root, "steps");
		break;
	default:
		return EINVAL;
	}
	int e = 0;
	struct json_object *item;
	for (size_t i = 0; !e && (item = jb_at(items, i)); ++i) {
		switch (api) {
		case AI_RESPONSES:
			if (jb_is(jb_get(item, "type"), "reasoning")) {
				e = ai_thought_parts(text, jb_get(item, "content"));
				e = e ? e : ai_thought_parts(text, jb_get(item, "summary"));
			}
			break;
		case AI_MESSAGES:
			if (jb_is(jb_get(item, "type"), "thinking"))
				e = ai_append(text, jb_get(item, "thinking"));
			break;
		case AI_GENERATE:
			if (json_object_get_boolean(jb_get(item, "thought")))
				e = ai_append(text, jb_get(item, "text"));
			break;
		case AI_INTERACTIONS:
			if (jb_is(jb_get(item, "type"), "thought"))
				e = ai_thought_parts(text, jb_get(item, "summary"));
			break;
		default:
			break;
		}
	}
	return e;
}

#undef no_answer_reason
