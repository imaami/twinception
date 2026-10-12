/** @file Upstream AI endpoint protocols: request bodies and answer parsing,
 *  whole or streamed. */
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "ai.h"
#include "dstr.h"
#include "jb.h"
#include "util.h"

/** Anthropic requires max_tokens; this fits every model's output limit. */
enum { ANTHROPIC_MAX_TOKENS = 4096 };

/* Each protocol's stop reason for an answer the output token limit cut. */
static char const *const limits[] = {
	[AI_OPENAI] = "length",
	[AI_RESPONSES] = "max_output_tokens",
	[AI_ANTHROPIC] = "max_tokens",
	[AI_GEMINI] = "MAX_TOKENS",
};

static struct json_object *
ai_message (char const         *role,
            struct json_object *content)
{
	return jb_object("role", json_object_new_string(role),
	                 "content", json_object_get(content),
	                 nullptr);
}

static struct json_object *
ai_gemini_content (char const         *role,
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

/* The model, the token limit under @p max_key, unless @p max_tokens is not
 * positive, and the temperature, into @p options, which is @p root or in it;
 * then --extra, merged into @p root. */
static int
ai_request_common (struct ai_endpoint const *ep,
                   struct json_object       *root,
                   struct json_object       *options,
                   char const               *max_key,
                   long                      max_tokens)
{
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

/* Chat completions' body.  Its messages go last, after what --extra added,
 * so that turns may follow them, unless --extra took them away; it streams
 * by the body, whatever --extra says. */
static int
ai_request_openai (struct ai_endpoint const *ep,
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
	    ai_request_common(ep, root, root, "max_tokens", ep->max_tokens) ||
	    jb_set(root, "stream", json_object_new_boolean(1)))
		return ENOMEM;
	messages = jb_get(root, "messages");
	if (!messages)
		return 0;
	json_object_get(messages);
	json_object_object_del(root, "messages");
	return jb_set(root, "messages", messages);
}

/* The Responses API's body, which streams by the body. */
static int
ai_request_responses (struct ai_endpoint const *ep,
                      struct json_object       *root,
                      struct json_object       *system,
                      struct json_object       *prompt)
{
	if (ai_request_model(ep, root) ||
	    jb_set(root, "input", json_object_get(prompt)) ||
	    (system && jb_set(root, "instructions", json_object_get(system))) ||
	    jb_set(root, "store", json_object_new_boolean(0)) ||
	    ai_request_common(ep, root, root, "max_output_tokens", ep->max_tokens))
		return ENOMEM;
	return jb_set(root, "stream", json_object_new_boolean(1));
}

/* Anthropic's body, which needs max_tokens and streams by the body. */
static int
ai_request_anthropic (struct ai_endpoint const *ep,
                      struct json_object       *root,
                      struct json_object       *system,
                      struct json_object       *prompt)
{
	long max_tokens = ep->max_tokens > 0 ? ep->max_tokens : ANTHROPIC_MAX_TOKENS;
	if (ai_request_model(ep, root) ||
	    jb_set(root, "messages", jb_list(ai_message("user", prompt))) ||
	    (system && jb_set(root, "system", json_object_get(system))) ||
	    ai_request_common(ep, root, root, "max_tokens", max_tokens))
		return ENOMEM;
	return jb_set(root, "stream", json_object_new_boolean(1));
}

/* Gemini's body, whose URL names the model and asks for a stream. */
static int
ai_request_gemini (struct ai_endpoint const *ep,
                   struct json_object       *root,
                   struct json_object       *system,
                   struct json_object       *prompt)
{
	if (jb_set(root, "contents", jb_list(ai_gemini_content("user", prompt))) ||
	    (system && jb_set(root, "systemInstruction",
	                      ai_gemini_content(nullptr, system))))
		return ENOMEM;
	struct json_object *options = root;
	if (ep->max_tokens > 0 || ep->temperature) {
		options = json_object_new_object();
		if (jb_set(root, "generationConfig", options))
			return ENOMEM;
	}
	return ai_request_common(ep, root, options, "maxOutputTokens", ep->max_tokens);
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
	switch (ep->protocol) {
	case AI_OPENAI:
		e = ai_request_openai(ep, root, system, prompt, tools);
		break;
	case AI_RESPONSES:
		e = ai_request_responses(ep, root, system, prompt);
		break;
	case AI_ANTHROPIC:
		e = ai_request_anthropic(ep, root, system, prompt);
		break;
	case AI_GEMINI:
		e = ai_request_gemini(ep, root, system, prompt);
		break;
	}
	if (e) {
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
ai_openai_answer (struct json_object  *root,
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
ai_anthropic_answer (struct json_object  *root,
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
ai_gemini_answer (struct json_object  *root,
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

/* Hand the pieces of chat completions' tool calls, @p calls, to @p sink. */
static int
ai_openai_calls (struct ai_sink const *sink,
                 struct json_object   *calls)
{
	if (!sink->call)
		return 0;
	int e = 0;
	struct json_object *c;
	for (size_t i = 0; !e && (c = jb_at(calls, i)); ++i) {
		struct json_object *index = jb_get(c, "index");
		struct json_object *fn = jb_get(c, "function");
		int64_t at = json_object_is_type(index, json_type_int)
		           ? json_object_get_int64(index) : -1;
		struct ai_call call = {
			.index = at >= 0 && at < AI_CALL_NEXT ? (uint32_t)at : AI_CALL_NEXT
		};
		call.id = ai_text_of(jb_get(c, "id"), &call.id_len);
		call.name = ai_text_of(jb_get(fn, "name"), &call.name_len);
		call.args = ai_text_of(jb_get(fn, "arguments"), &call.args_len);
		e = sink->call(sink->user, &call);
	}
	return e;
}

/* The tokens that @p usage, chat completions', says the request and the
 * answer took; 0 if it says nothing of them. */
static uint32_t
ai_openai_used (struct json_object *usage)
{
	int64_t prompt = json_object_get_int64(jb_get(usage, "prompt_tokens"));
	int64_t answer = json_object_get_int64(jb_get(usage, "completion_tokens"));
	int64_t n = prompt > 0 && answer >= 0 ? prompt + answer : 0;
	return n < UINT32_MAX ? (uint32_t)n : UINT32_MAX;
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
	bool erred = error || jb_is(type, "error");
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
ai_cut (enum ai_protocol  protocol,
        char const       *reason)
{
	return reason && !strcmp(reason, limits[protocol]);
}

/* Hand the pieces of chat completions' @p delta to @p sink, saying in @p out
 * whether it carries thinking. */
static int
ai_openai_delta (struct ai_sink const *sink,
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
	return ai_openai_calls(sink, jb_get(delta, "tool_calls"));
}

/* Hand the text of Anthropic's content block @p delta to @p sink, saying in
 * @p out whether it carries thinking. */
static int
ai_anthropic_delta (struct ai_sink const *sink,
                    struct json_object   *delta,
                    struct ai_event      *out)
{
	struct json_object *kind = jb_get(delta, "type");
	if (jb_is(kind, "text_delta"))
		return ai_put_text(sink->text, sink->user, jb_get(delta, "text"));
	out->thought = jb_is(kind, "thinking_delta") &&
	               ai_some_text(jb_get(delta, "thinking"));
	return 0;
}

int
ai_event (enum ai_protocol       protocol,
          struct json_object    *ev,
          struct ai_sink const  *sink,
          struct ai_event       *out)
{
	if ((out->error = ai_event_error(ev)))
		return 0;
	int (*put)(void *, char const *, size_t) = sink->text;
	void *user = sink->user;
	int e = 0;
	struct json_object *type = jb_get(ev, "type");
	struct json_object *reason = nullptr;
	switch (protocol) {
	case AI_OPENAI: {
		/* The first completion's, should --extra ask for more (n). */
		struct json_object *choices = jb_get(ev, "choices");
		struct json_object *choice;
		for (size_t i = 0; (choice = jb_at(choices, i)) &&
		                   json_object_get_int64(jb_get(choice, "index")); ++i)
			;
		/* With include_usage, a last event of its own, with no choices. */
		out->used = ai_openai_used(jb_get(ev, "usage"));
		reason = jb_get(choice, "finish_reason");
		e = ai_openai_delta(sink, jb_get(choice, "delta"), out);
		break;
	}
	case AI_RESPONSES:
		if (jb_is(type, "response.output_text.delta") ||
		    jb_is(type, "response.refusal.delta"))
			e = ai_put_text(put, user, jb_get(ev, "delta"));
		out->thought = (jb_is(type, "response.reasoning_text.delta") ||
		                jb_is(type, "response.reasoning_summary_text.delta")) &&
		               ai_some_text(jb_get(ev, "delta"));
		reason = jb_get(jb_get(jb_get(ev, "response"), "incomplete_details"),
		                "reason");
		break;
	case AI_ANTHROPIC: {
		struct json_object *delta = jb_get(ev, "delta");
		reason = jb_get(delta, "stop_reason");
		if (jb_is(type, "content_block_delta"))
			e = ai_anthropic_delta(sink, delta, out);
		break;
	}
	case AI_GEMINI: {
		struct json_object *candidate = jb_at(jb_get(ev, "candidates"), 0);
		struct json_object *parts = jb_get(jb_get(candidate, "content"), "parts");
		struct json_object *part;
		for (size_t i = 0; !e && (part = jb_at(parts, i)); ++i)
			if (!json_object_get_boolean(jb_get(part, "thought")))
				e = ai_put_text(put, user, jb_get(part, "text"));
			else
				out->thought |= ai_some_text(jb_get(part, "text"));
		reason = candidate ? jb_get(candidate, "finishReason")
		       : jb_get(jb_get(ev, "promptFeedback"), "blockReason");
		break;
	}
	}
	out->reason = jb_text(reason);
	out->truncated = ai_cut(protocol, out->reason);
	out->end = out->reason || jb_is(type, "response.completed") ||
	           jb_is(type, "response.incomplete") || jb_is(type, "message_stop");
	return e;
}

struct ai_format const *
ai_format (enum ai_protocol protocol)
{
#define B(s) { s, sizeof s - 1 }
	static struct ai_format const chat = {
		.turn       = B(",{\"role\":\"assistant\",\"content\":\""),
		.thought    = B("\",\"reasoning_content\":\""),
		.calls      = B("\",\"tool_calls\":["),
		.call       = B("{\"id\":\""),
		.call_next  = B(",{\"id\":\""),
		.name       = B("\",\"type\":\"function\",\"function\":{\"name\":\""),
		.args       = B("\",\"arguments\":\""),
		.call_end   = B("\"}}"),
		.turn_end   = B("]}"),
		.result     = B(",{\"role\":\"tool\",\"tool_call_id\":\""),
		.output     = B("\",\"content\":\""),
		.result_end = B("\"}"),
		.prompt_end = B("\"}"),
		.end        = B("]}"),
	};
#undef B
	return protocol == AI_OPENAI ? &chat : nullptr;
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
ai_answer (enum ai_protocol    protocol,
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
	switch (protocol) {
	case AI_OPENAI:
		e = ai_openai_answer(root, text, &reason);
		break;
	case AI_RESPONSES:
		e = ai_responses_answer(root, text, &reason);
		break;
	case AI_ANTHROPIC:
		e = ai_anthropic_answer(root, text, &reason);
		break;
	case AI_GEMINI:
		e = ai_gemini_answer(root, text, &reason);
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
	if (ai_cut(protocol, reason))
		return dstr_cat_n(text, AI_CUT_NOTE, sizeof AI_CUT_NOTE - 1) ? 0 : ENOMEM;
	return 0;
}

#undef no_answer_reason
