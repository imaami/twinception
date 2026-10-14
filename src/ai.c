/** @file Upstream AI APIs: request bodies and answer parsing,
 *  whole or streamed. */
#include <errno.h>
#include <stdckdint.h>
#include <stdio.h>
#include <string.h>

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

/* The type of the step whose item is open in the replay, in Gemini's
 * Interactions, or the end of the answer, after which no event changes the
 * replay (struct ai_reader's kind). */
enum { INTERACTIONS_THOUGHT, INTERACTIONS_OUTPUT, INTERACTIONS_ENDED };

/* The state of the summary array of the open thought (struct ai_reader's
 * summary). */
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

/* A copy of @p val, which the configuration holds: every loop reads it, so no
 * request may take a reference, whose count is not atomic.  nullptr if out of
 * memory. */
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
 * positive, and the temperature into @p root.  If @p config is not nullptr,
 * write them into a new object, @p root's member @p config, which is added
 * only if one of them is set.  Then merge --extra into @p root. */
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

/* The model, if @p ep names one, first in @p root. */
static int
ai_request_model (struct ai_endpoint const *ep,
                  struct json_object       *root)
{
	return ep->model ? jb_set(root, "model", json_object_new_string(ep->model)) : 0;
}

/* Chat completions' body, which streams by the body, whatever --extra
 * says. */
static int
ai_request_chat (struct ai_endpoint const *ep,
                 struct json_object       *root,
                 struct json_object       *system,
                 struct json_object       *prompt,
                 struct json_object       *tools)
{
	struct json_object *messages = json_object_new_array();
	if (ai_request_model(ep, root) ||
	    jb_set(root, "messages", messages) ||
	    (system && jb_push(messages, ai_message("system", system))) ||
	    jb_push(messages, ai_message("user", prompt)) ||
	    (tools && (jb_set(root, "tools", json_object_get(tools)) ||
	               jb_set(root, "stream_options", jb_object(
	                      "include_usage", json_object_new_boolean(1),
	                      nullptr)))) ||
	    ai_request_common(ep, root, nullptr, "max_tokens", ep->max_tokens))
		return ENOMEM;
	return jb_set(root, "stream", json_object_new_boolean(1));
}

/* The Responses API's body, which streams by the body. */
static int
ai_request_responses (struct ai_endpoint const *ep,
                      struct json_object       *root,
                      struct json_object       *system,
                      struct json_object       *prompt,
                      struct json_object       *tools)
{
	if (ai_request_model(ep, root) ||
	    jb_set(root, "input", jb_list(ai_message("user", prompt))) ||
	    (system && jb_set(root, "instructions", json_object_get(system))) ||
	    (tools && jb_set(root, "tools", json_object_get(tools))) ||
	    jb_set(root, "store", json_object_new_boolean(0)) ||
	    ai_request_common(ep, root, nullptr, "max_output_tokens", ep->max_tokens))
		return ENOMEM;
	return jb_set(root, "stream", json_object_new_boolean(1));
}

/* Anthropic's body, which needs max_tokens and streams by the body. */
static int
ai_request_messages (struct ai_endpoint const *ep,
                     struct json_object       *root,
                     struct json_object       *system,
                     struct json_object       *prompt,
                     struct json_object       *tools)
{
	long max_tokens = ep->max_tokens > 0 ? ep->max_tokens : MESSAGES_MAX_TOKENS;
	if (ai_request_model(ep, root) ||
	    jb_set(root, "messages", jb_list(ai_message("user", prompt))) ||
	    (system && jb_set(root, "system", json_object_get(system))) ||
	    (tools && jb_set(root, "tools", json_object_get(tools))) ||
	    ai_request_common(ep, root, nullptr, "max_tokens", max_tokens))
		return ENOMEM;
	return jb_set(root, "stream", json_object_new_boolean(1));
}

/* Gemini's body, whose URL names the model and asks for a stream. */
static int
ai_request_generate (struct ai_endpoint const *ep,
                     struct json_object       *root,
                     struct json_object       *system,
                     struct json_object       *prompt,
                     struct json_object       *tools)
{
	if (jb_set(root, "contents", jb_list(ai_generate_content("user", prompt))) ||
	    (system && jb_set(root, "systemInstruction",
	                      ai_generate_content(nullptr, system))) ||
	    (tools && jb_set(root, "tools", json_object_get(tools))))
		return ENOMEM;
	return ai_request_common(ep, root, "generationConfig", "maxOutputTokens",
	                         ep->max_tokens);
}

/* Return a user_input step of Gemini's Interactions with the text @p text.
 * The text is the last value of each object and array around it, so that
 * the body ends after the text with prompt_end and end (ai_format()). */
static struct json_object *
ai_interactions_input (struct json_object *text)
{
	struct json_object *content = jb_object("type", json_object_new_string("text"),
	                                        "text", json_object_get(text), nullptr);
	return jb_object("type", json_object_new_string("user_input"),
	                 "content", jb_list(content), nullptr);
}

/* The request body of Gemini's Interactions.  stream is set after --extra
 * is merged, so that the answer is always streamed. */
static int
ai_request_interactions (struct ai_endpoint const *ep,
                         struct json_object       *root,
                         struct json_object       *system,
                         struct json_object       *prompt,
                         struct json_object       *tools)
{
	if (ai_request_model(ep, root) ||
	    jb_set(root, "input", jb_list(ai_interactions_input(prompt))) ||
	    (system && jb_set(root, "system_instruction", json_object_get(system))) ||
	    (tools && jb_set(root, "tools", json_object_get(tools))) ||
	    jb_set(root, "store", json_object_new_boolean(0)) ||
	    ai_request_common(ep, root, "generation_config", "max_output_tokens",
	                      ep->max_tokens))
		return ENOMEM;
	return jb_set(root, "stream", json_object_new_boolean(1));
}

/* Move the conversation, @p root's member @p key, to its end, after what
 * --extra added, so that turns may follow it, unless --extra took it away. */
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
            struct json_object       *tools)
{
	struct json_object *root = json_object_new_object();
	if (!root)
		return nullptr;
	int e = 0;
	switch (ep->api) {
	case AI_CHAT:
		e = ai_request_chat(ep, root, system, prompt, tools);
		break;
	case AI_RESPONSES:
		e = ai_request_responses(ep, root, system, prompt, tools);
		break;
	case AI_MESSAGES:
		e = ai_request_messages(ep, root, system, prompt, tools);
		break;
	case AI_GENERATE:
		e = ai_request_generate(ep, root, system, prompt, tools);
		break;
	case AI_INTERACTIONS:
		e = ai_request_interactions(ep, root, system, prompt, tools);
		break;
	}
	if (e || ai_request_last(root, ai_format(ep->api)->conversation)) {
		json_object_put(root);
		return nullptr;
	}
	return root;
}

/* Append @p val's text to @p text, if it is a string. */
static int
ai_append (struct dstr        *text,
           struct json_object *val)
{
	char const *str = jb_text(val);
	return str && !dstr_cat_n(text, str, (size_t)json_object_get_string_len(val))
	       ? ENOMEM : 0;
}

/* The text of chat completions' answer @p root, into @p text, and its stop
 * reason in @p *reason. */
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
	int e = 0;
	struct json_object *item;
	for (size_t i = 0; !e && (item = jb_at(jb_get(root, "output"), i)); ++i) {
		if (!jb_is(jb_get(item, "type"), "message"))
			continue;
		struct json_object *part;
		for (size_t j = 0; !e && (part = jb_at(jb_get(item, "content"), j)); ++j) {
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
	int e = 0;
	struct json_object *block;
	for (size_t i = 0; !e && (block = jb_at(jb_get(root, "content"), i)); ++i)
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
 * @p *reason. */
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

/* Hand @p val's text to @p put, if it is a string and @p put is not
 * nullptr. */
static int
ai_put_text (int               (*put) (void       *user,
                                       char const *text,
                                       size_t      len),
             void               *user,
             struct json_object *val)
{
	char const *str = put ? jb_text(val) : nullptr;
	return str ? put(user, str, (size_t)json_object_get_string_len(val)) : 0;
}

/* @p val's text and its length in @p *len, if it is a string; else
 * nullptr. */
static char const *
ai_text_of (struct json_object *val,
            size_t             *len)
{
	char const *str = jb_text(val);
	*len = str ? (size_t)json_object_get_string_len(val) : 0;
	return str;
}

/* @p val as a call's index, or a content block's: AI_CALL_NEXT if it is
 * none. */
static uint32_t
ai_index (struct json_object *val)
{
	int64_t at = json_object_is_type(val, json_type_int) ? json_object_get_int64(val)
	                                                     : -1;
	return at >= 0 && at < AI_CALL_NEXT ? (uint32_t)at : AI_CALL_NEXT;
}

/* Return the sum of @p prompt, the token count of a request, and @p answer,
 * the token count of its answer.  Return UINT32_MAX if the sum is larger
 * than UINT32_MAX.  Return 0, which means that the count is not known, if
 * @p prompt is not positive, if @p answer is negative, or if the sum
 * overflows int64_t. */
static uint32_t
ai_used (int64_t prompt,
         int64_t answer)
{
	int64_t n;
	if (prompt <= 0 || answer < 0 || ckd_add(&n, prompt, answer))
		return 0;
	return n < UINT32_MAX ? (uint32_t)n : UINT32_MAX;
}

/* Return @p val as a token count if it is a JSON integer, else 0.  json-c
 * also parses NaN, which json_object_get_int64() cannot convert.  ai_used()
 * treats a request count of 0 as not known and an answer count of 0 as 0. */
static int64_t
ai_count (struct json_object *val)
{
	return json_object_is_type(val, json_type_int) ? json_object_get_int64(val) : 0;
}

/* The place of @p type, an event's or an item's, among the @p n @p names; @p n
 * if it is none of them. */
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

/* Give @p sink's replay, if it keeps one, the @p len bytes of JSON text at
 * @p json. */
static int
ai_replay (struct ai_sink const *sink,
           char const           *json,
           size_t                len)
{
	return sink->replay ? sink->replay(sink->user, json, len) : 0;
}

/* Give it the text of @p val, if that is a string, escaped as a JSON
 * string's. */
static int
ai_replay_text (struct ai_sink const *sink,
                struct json_object   *val)
{
	if (!sink->replay || !json_object_is_type(val, json_type_string))
		return 0;
	size_t len = 0;
	char const *json = json_object_to_json_string_length(val, JB_WIRE, &len);
	return json ? sink->replay(sink->user, json + 1, len - 2) : ENOMEM;
}

/* Pass @p val, an item of the answer, whole, to @p sink's replay, if it has
 * one, after the @p len bytes at @p head. */
static int
ai_replay_after (struct ai_sink const *sink,
                 char const           *head,
                 size_t                len,
                 struct json_object   *val)
{
	if (!sink->replay)
		return 0;
	size_t n = 0;
	char const *json = json_object_to_json_string_length(val, JB_WIRE, &n);
	if (!json)
		return ENOMEM;
	int e = sink->replay(sink->user, head, len);
	return e ? e : sink->replay(sink->user, json, n);
}

/* Give it @p val, an item of the answer, whole, after a comma. */
static int
ai_replay_item (struct ai_sink const *sink,
                struct json_object   *val)
{
	return ai_replay_after(sink, ",", 1, val);
}

/* Hand the pieces of chat completions' tool calls, @p calls, to @p sink. */
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
		struct ai_call call = { .index = ai_index(jb_get(c, "index")) };
		/* Gemini's thought signature, which a turn gives back. */
		struct json_object *extra = jb_get(c, "extra_content");
		call.id = ai_text_of(jb_get(c, "id"), &call.id_len);
		call.name = ai_text_of(jb_get(fn, "name"), &call.name_len);
		call.extra = extra ? json_object_to_json_string_length(extra, JB_WIRE,
		                                                       &call.extra_len)
		                   : nullptr;
		call.args = ai_text_of(jb_get(fn, "arguments"), &call.args_len);
		e = extra && !call.extra ? ENOMEM : sink->call(sink->user, &call);
	}
	return e;
}

/* Whether @p val is a string with something in it. */
static bool
ai_some_text (struct json_object *val)
{
	return jb_text(val) && json_object_get_string_len(val);
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

/* Hand the pieces of chat completions' @p delta to @p sink, saying in @p out
 * whether it carries thinking. */
static int
ai_chat_delta (struct ai_sink const *sink,
               struct json_object   *delta,
               struct ai_event      *out)
{
	struct json_object *content = jb_get(delta, "content");
	/* Some servers send content as an array of text parts. */
	int e = ai_put_text(sink->text, sink->user, content);
	struct json_object *part;
	for (size_t i = 0; !e && (part = jb_at(content, i)); ++i)
		e = ai_put_text(sink->text, sink->user, jb_get(part, "text"));
	if (e || (e = ai_put_text(sink->text, sink->user, jb_get(delta, "refusal"))))
		return e;
	/* llama-server's and DeepSeek's name, and OpenRouter's; only the first
	 * goes back in a turn, as reasoning_content. */
	struct json_object *thinking = jb_get(delta, "reasoning_content");
	bool kept = ai_some_text(thinking);
	out->thought = kept || ai_some_text(jb_get(delta, "reasoning"));
	if (kept && (e = ai_put_text(sink->thought, sink->user, thinking)))
		return e;
	return ai_chat_calls(sink, jb_get(delta, "tool_calls"));
}

/* Hand @p item, an output item of the Responses API, @p at in the output,
 * to @p sink: a function call as a call, and a call, a reasoning item or a
 * message, whole, to the replay. */
static int
ai_responses_item (struct ai_sink const *sink,
                   struct json_object   *at,
                   struct json_object   *item)
{
	static char const *const kinds[] = { "function_call", "reasoning", "message" };
	size_t kind = ai_kind(jb_get(item, "type"), kinds, ARRAY_SIZE(kinds));
	if (kind == ARRAY_SIZE(kinds))
		return 0;
	int e = 0;
	if (!kind && sink->call) {
		struct ai_call call = { .index = ai_index(at) };
		call.id = ai_text_of(jb_get(item, "call_id"), &call.id_len);
		call.name = ai_text_of(jb_get(item, "name"), &call.name_len);
		call.args = ai_text_of(jb_get(item, "arguments"), &call.args_len);
		e = sink->call(sink->user, &call);
	}
	return e ? e : ai_replay_item(sink, item);
}

/* Hand the pieces of the Responses API's event @p ev to @p sink, saying in
 * @p out whether it carries thinking, and the end and the tokens used once
 * it ends. */
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
		return ai_put_text(sink->text, sink->user, delta);
	case REASONING:
	case SUMMARY:
		out->thought = ai_some_text(delta);
		return 0;
	case ITEM:
		return ai_responses_item(sink, jb_get(ev, "output_index"),
		                         jb_get(ev, "item"));
	case DONE:
	case CUT: {
		struct json_object *usage = jb_get(jb_get(ev, "response"), "usage");
		out->end = 1;
		out->used = ai_used(json_object_get_int64(jb_get(usage, "input_tokens")),
		                    json_object_get_int64(jb_get(usage, "output_tokens")));
		return 0;
	}
	}
	return 0;
}

#define REPLAY(s) ai_replay(sink, s, sizeof s - 1)

/* Hand the start of tool call @p at, @p item, to @p sink: its id and name,
 * and its arguments, @p item's member @p key, if they are a JSON object that
 * is not empty.  A server that sends the arguments whole sends them in the
 * start. */
static int
ai_call_start (struct ai_sink const *sink,
               uint32_t              at,
               struct json_object   *item,
               char const           *key)
{
	if (!sink->call)
		return 0;
	struct json_object *args = jb_get(item, key);
	bool whole = json_object_is_type(args, json_type_object) &&
	             json_object_object_length(args);
	struct ai_call call = { .index = at };
	call.id = ai_text_of(jb_get(item, "id"), &call.id_len);
	call.name = ai_text_of(jb_get(item, "name"), &call.name_len);
	call.args = whole ? json_object_to_json_string_length(args, JB_WIRE,
	                                                      &call.args_len)
	                  : nullptr;
	return whole && !call.args ? ENOMEM : sink->call(sink->user, &call);
}

/* Hand what Anthropic's content block @p block, @p at in the content,
 * begins with to @p sink: a tool call's id and name; or, to the replay, the
 * start of a thinking block, which @p reader then has open, or a redacted
 * one whole. */
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
		return ai_call_start(sink, at, block, "input");
	case THINKING:
		reader->open = at + 1;
		return REPLAY(",{\"type\":\"thinking\",\"thinking\":\"");
	case REDACTED:
		return ai_replay_item(sink, block);
	}
	return 0;
}

/* Hand the pieces of @p delta, of Anthropic's content block @p at, to
 * @p sink: its text, its thinking's and its signature, which go on in the
 * replay with the block's item that @p reader has open, or a text block's
 * first text that is not white space, which opens its item, as a turn may
 * have no blank one; or a piece of a tool call's arguments.  Says in @p out
 * whether it carries thinking. */
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
		char const *str = jb_text(text);
		int e = ai_put_text(sink->text, sink->user, text);
		if (e || (!open && (!str || !str[strspn(str, " \t\r\n")])))
			return e;
		if (!open) {
			reader->open = at + 1;
			e = REPLAY(",{\"type\":\"text\",\"text\":\"");
		}
		return e ? e : ai_replay_text(sink, text);
	}
	case THINKING: {
		struct json_object *text = jb_get(delta, "thinking");
		out->thought = ai_some_text(text);
		return open ? ai_replay_text(sink, text) : 0;
	}
	case SIGNATURE: {
		if (!open)
			return 0;
		int e = REPLAY("\",\"signature\":\"");
		return e ? e : ai_replay_text(sink, jb_get(delta, "signature"));
	}
	case INPUT: {
		if (!sink->call)
			return 0;
		struct ai_call call = { .index = at };
		call.args = ai_text_of(jb_get(delta, "partial_json"), &call.args_len);
		return sink->call(sink->user, &call);
	}
	}
	return 0;
}

/* Hand the pieces of Anthropic's event @p ev to @p sink, as @p reader keeps
 * the reading, saying in @p out whether it carries thinking, the tokens
 * used as the answer ends, and the end of the message.  An event of a
 * content block with no index may carry text only. */
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
	if (kind < MESSAGE && at == AI_CALL_NEXT)
		return kind == DELTA ? ai_put_text(sink->text, sink->user,
		                                   jb_get(jb_get(ev, "delta"), "text"))
		                     : 0;
	switch (kind) {
	case DELTA:
		return ai_messages_delta(sink, reader, at, jb_get(ev, "delta"), out);
	case START:
		return ai_messages_start(sink, reader, at, jb_get(ev, "content_block"));
	case STOP:
		if (reader->open != at + 1)
			return 0;
		reader->open = 0;
		return REPLAY("\"}");
	case MESSAGE: {
		struct json_object *usage = jb_get(jb_get(ev, "message"), "usage");
		struct json_object *written = jb_get(usage, "cache_creation_input_tokens");
		struct json_object *cached = jb_get(usage, "cache_read_input_tokens");
		int64_t n = json_object_get_int64(jb_get(usage, "input_tokens"));
		/* A sum that overflows int64_t is not a token count. */
		bool over = ckd_add(&n, n, json_object_get_int64(written)) ||
		            ckd_add(&n, n, json_object_get_int64(cached));
		reader->input = over ? 0 : ai_used(n, 0);
		return 0;
	}
	case MESSAGE_DELTA: {
		struct json_object *usage = jb_get(ev, "usage");
		out->used = ai_used(reader->input,
		                    json_object_get_int64(jb_get(usage, "output_tokens")));
		return 0;
	}
	case MESSAGE_STOP:
		out->end = 1;
		return 0;
	}
	return 0;
}

/* End the text part that @p reader has open in @p sink's replay, if any. */
static int
ai_generate_close (struct ai_sink const *sink,
                   struct ai_reader     *reader)
{
	if (!reader->open)
		return 0;
	reader->open = 0;
	return REPLAY("\"}");
}

/* Hand Gemini's function call @p fc to @p sink, numbered as @p reader
 * counts the calls. */
static int
ai_generate_call (struct ai_sink const *sink,
                  struct ai_reader     *reader,
                  struct json_object   *fc)
{
	if (!sink->call)
		return 0;
	struct json_object *args = jb_get(fc, "args");
	struct ai_call call = { .index = reader->calls++ };
	call.id = ai_text_of(jb_get(fc, "id"), &call.id_len);
	call.name = ai_text_of(jb_get(fc, "name"), &call.name_len);
	call.args = args ? json_object_to_json_string_length(args, JB_WIRE,
	                                                     &call.args_len)
	                 : nullptr;
	return args && !call.args ? ENOMEM : sink->call(sink->user, &call);
}

/* End the text part that @p reader has open, then give @p sink's replay
 * @p part whole. */
static int
ai_generate_whole (struct ai_sink const *sink,
                   struct ai_reader     *reader,
                   struct json_object   *part)
{
	int e = ai_generate_close(sink, reader);
	return e ? e : ai_replay_item(sink, part);
}

/* Hand Gemini's part @p part to @p sink, as @p reader keeps the reading: a
 * function call, which goes whole to the replay, as does a part with a
 * thought signature, in which it came; or text, which goes on in the text
 * part open in the replay, or thinking, as @p out says. */
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
	struct json_object *signature = jb_get(part, "thoughtSignature");
	if (json_object_get_boolean(jb_get(part, "thought"))) {
		out->thought |= ai_some_text(text);
		return signature ? ai_generate_whole(sink, reader, part) : 0;
	}
	int e = ai_put_text(sink->text, sink->user, text);
	if (e)
		return e;
	if (signature)
		return ai_generate_whole(sink, reader, part);
	if (!ai_some_text(text))
		return 0;
	if (!reader->open) {
		reader->open = 1;
		e = REPLAY(",{\"text\":\"");
	}
	return e ? e : ai_replay_text(sink, text);
}

/* Close the open item in @p sink's replay, if there is one (@p reader).
 * For a thought, close its summary array if it is open, then the thought.
 * For a model output, close its text and the step. */
static int
ai_interactions_close (struct ai_sink const *sink,
                       struct ai_reader     *reader)
{
	if (!reader->open)
		return 0;
	reader->open = 0;
	if (reader->kind == INTERACTIONS_OUTPUT)
		return REPLAY("\"}]}");
	return reader->summary == SUMMARY_OPEN ? REPLAY("]}") : REPLAY("}");
}

/* Pass @p text, a piece of text of step @p at of Gemini's Interactions, to
 * @p sink's text and to its replay.  The text goes into the open item if
 * that item is the model output of step @p at.  If no item is open, text
 * that is not only white space starts a new model output item for step
 * @p at.  Otherwise the replay does not get the text. */
static int
ai_interactions_text (struct ai_sink const *sink,
                      struct ai_reader     *reader,
                      uint32_t              at,
                      struct json_object   *text)
{
	size_t len;
	char const *str = ai_text_of(text, &len);
	int e = str && sink->text ? sink->text(sink->user, str, len) : 0;
	if (e || !str)
		return e;
	if (reader->open == at + 1)
		return reader->kind == INTERACTIONS_OUTPUT ? ai_replay_text(sink, text) : 0;
	if (reader->open || strspn(str, " \t\r\n") == len)
		return 0;
	reader->open = at + 1;
	reader->kind = INTERACTIONS_OUTPUT;
	e = REPLAY(",{\"type\":\"model_output\",\"content\":[{\"type\":\"text\",\"text\":\"");
	return e ? e : ai_replay_text(sink, text);
}

/* Write @p item, a summary item of the open thought, whole to @p sink's
 * replay: after the earlier items, or in a new summary array.  Skip an item
 * that is not a JSON object, and an item that comes after the array is
 * closed. */
static int
ai_interactions_summary (struct ai_sink const *sink,
                         struct ai_reader     *reader,
                         struct json_object   *item)
{
	if (reader->summary == SUMMARY_CLOSED || !json_object_is_type(item, json_type_object))
		return 0;
	if (reader->summary == SUMMARY_OPEN)
		return ai_replay_item(sink, item);
	reader->summary = SUMMARY_OPEN;
	static char const head[] = ",\"summary\":[";
	return ai_replay_after(sink, head, sizeof head - 1, item);
}

/* Write @p signature into the open thought in @p sink's replay.  Close the
 * summary array first if it is open.  Skip a signature that is not a
 * string, that is empty, or that comes after the thought already has a
 * signature. */
static int
ai_interactions_sign (struct ai_sink const *sink,
                      struct ai_reader     *reader,
                      struct json_object   *signature)
{
	if (reader->sign || !ai_some_text(signature))
		return 0;
	reader->sign = 1;
	int e = 0;
	if (reader->summary == SUMMARY_OPEN) {
		reader->summary = SUMMARY_CLOSED;
		e = REPLAY("]");
	}
	e = e ? e : REPLAY(",\"signature\":\"");
	e = e ? e : ai_replay_text(sink, signature);
	return e ? e : REPLAY("\"");
}

/* Start the item of @p step, thought step @p at of Gemini's Interactions,
 * in @p sink's replay, and mark it open in @p reader.  Write its type, then
 * the signature and the summary items that the start event contains.  Set
 * out->thought if a summary item has text. */
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
	int e = REPLAY(",{\"type\":\"thought\"");
	e = e ? e : ai_interactions_sign(sink, reader, jb_get(step, "signature"));
	struct json_object *summary = jb_get(step, "summary");
	struct json_object *item;
	for (size_t i = 0; !e && (item = jb_at(summary, i)); ++i) {
		out->thought |= ai_some_text(jb_get(item, "text"));
		e = ai_interactions_summary(sink, reader, item);
	}
	return e;
}

/* Process @p step, the start of step @p at of Gemini's Interactions.
 * Ignore a step of a type other than thought, model_output and
 * function_call, such as the step of a server tool; the open item stays
 * open.  For those three types, close the open item in the replay first.
 * Then start the item of a thought (ai_interactions_thought()), pass the
 * text of a model output (ai_interactions_text()), or mark the function
 * call as the last that started in @p reader and pass it to @p sink. */
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
	case CALL:
		reader->calls = at + 1;
		return ai_call_start(sink, at, step, "arguments");
	}
	return 0;
}

/* Process @p delta, a delta of step @p at of Gemini's Interactions.  Pass
 * the text of a model output (ai_interactions_text()).  Write a summary
 * item or a signature into the thought of step @p at if that thought is
 * open, and set out->thought if the summary item has text.  Pass a piece of
 * the arguments of a function call to @p sink.  If step @p at is not the
 * function call that started last (@p reader), close the open item in the
 * replay first, because a piece that starts a new call places the call at
 * the current end of the replay, and mark step @p at as that call. */
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
		out->thought = ai_some_text(jb_get(item, "text"));
		return thought ? ai_interactions_summary(sink, reader, item) : 0;
	}
	case SIGNATURE:
		return thought ? ai_interactions_sign(sink, reader, jb_get(delta, "signature"))
		               : 0;
	case ARGUMENTS: {
		struct ai_call call = { .index = at };
		call.args = ai_text_of(jb_get(delta, "arguments"), &call.args_len);
		if (!sink->call || !call.args)
			return 0;
		int e = reader->calls == at + 1 ? 0 : ai_interactions_close(sink, reader);
		reader->calls = at + 1;
		return e ? e : sink->call(sink->user, &call);
	}
	}
	return 0;
}

/* Process @p ev, an event of Gemini's Interactions of type @p type, with
 * the state of the reading in @p reader.  interaction.completed closes the
 * open item in the replay, sets out->end and the tokens used, and makes
 * the reading ignore the events after it.  A step event without an index
 * can pass only text to @p sink. */
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
	if (kind < DONE && at == AI_CALL_NEXT) {
		struct json_object *text = kind == DELTA ? jb_get(delta, "text") : nullptr;
		return ai_put_text(sink->text, sink->user, text);
	}
	switch (kind) {
	case DELTA:
		return ai_interactions_delta(sink, reader, at, delta, out);
	case START:
		return ai_interactions_start(sink, reader, at, jb_get(ev, "step"), out);
	case STOP:
		return reader->open == at + 1 ? ai_interactions_close(sink, reader) : 0;
	case DONE: {
		/* The thinking is not sent back in the next request, so the tokens
		 * used are total_input_tokens plus total_output_tokens, which leave
		 * out the thinking.  If that sum is not known, the tokens used are
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

#undef REPLAY

int
ai_event (enum ai_api            api,
          struct ai_reader      *reader,
          struct json_object    *ev,
          struct ai_sink const  *sink,
          struct ai_event       *out)
{
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
		                   json_object_get_int64(jb_get(choice, "index")); ++i)
			;
		/* With include_usage, a last event of its own, with no choices. */
		struct json_object *usage = jb_get(ev, "usage");
		out->used = ai_used(json_object_get_int64(jb_get(usage, "prompt_tokens")),
		                    json_object_get_int64(jb_get(usage, "completion_tokens")));
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
		out->used = ai_used(json_object_get_int64(jb_get(usage, "totalTokenCount")), 0);
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
#define B(s) { s, sizeof s - 1 }
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
		 * their thought signatures. */
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
			.user         = B(",{\"role\":\"user\",\"parts\":[{\"text\":\""),
			.prompt_end   = B("\"}]}"),
			.end          = B("]}"),
			.conversation = "contents",
			.flags        = AI_RESULT_NAMED,
		},
		/* A turn is the replay's steps and the calls, in the input list.
		 * Each result is a step after them. */
		[AI_INTERACTIONS] = {
			.tools        = B("["),
			.tools_end    = B("]"),
			.call         = B(",{\"type\":\"function_call\",\"id\":\""),
			.call_next    = B(",{\"type\":\"function_call\",\"id\":\""),
			.name         = B("\",\"name\":\""),
			.args         = B("\",\"arguments\":"),
			.call_end     = B("}"),
			.result       = B(",{\"type\":\"function_result\",\"call_id\":\""),
			.output       = B("\",\"result\":\""),
			.result_end   = B("\"}"),
			.user         = B(",{\"type\":\"user_input\",\"content\":"
			                  "[{\"type\":\"text\",\"text\":\""),
			.prompt_end   = B("\"}]}"),
			.end          = B("]}"),
			.conversation = "input",
			.flags        = AI_ARGS_RAW,
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
		if (tool && jb_set(tool, "type", json_object_new_string("function"))) {
			json_object_put(tool);
			return nullptr;
		}
		return tool;
	case AI_MESSAGES:
	case AI_GENERATE:
		break;
	}
	return tool;
}

/* Say in @p text what the upstream's error answer, of HTTP @p status, says:
 * its message, or the start of the @p len bytes at @p body. */
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

	/* Quote the start of an unparsable body, as far as it is UTF-8 text. */
	size_t cut = jb_utf8(body, len < 512 ? len : 512);
#define QUOTE(s) dstr_cat_n(text, s, sizeof s - 1)
	bool ok = cut ? dstr_cat_n(text, body, cut)
	        : len ? QUOTE("a response that is not text")
	              : QUOTE("empty response");
#undef QUOTE
	return ok ? 0 : ENOMEM;
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

#undef no_answer_reason
