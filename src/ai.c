/** @file Upstream AI endpoint protocols: request bodies and answer parsing,
 *  whole or streamed. */
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "ai.h"
#include "dstr.h"
#include "jb.h"

/** Anthropic requires max_tokens; this fits every model's output limit. */
enum { ANTHROPIC_MAX_TOKENS = 4096 };

struct answer {
	char const *reason;
	bool        truncated;
};

static struct json_object *
message (char const         *role,
         struct json_object *content)
{
	return jb_object("role", json_object_new_string(role),
	                 "content", json_object_get(content),
	                 nullptr);
}

static struct json_object *
gemini_content (char const         *role,
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
copy (struct json_object *val)
{
	struct json_object *dup = nullptr;
	return json_object_deep_copy(val, &dup, nullptr) ? nullptr : dup;
}

/* Deep-merge @p src into @p dst; a null value removes the key. */
static int
merge (struct json_object *dst,
       struct json_object *src)
{
	struct json_object_iter it;
	json_object_object_foreachC(src, it) {
		struct json_object *cur = jb_get(dst, it.key);
		if (!it.val) {
			json_object_object_del(dst, it.key);
		} else if (json_object_is_type(it.val, json_type_object) &&
		           json_object_is_type(cur, json_type_object)) {
			if (merge(cur, it.val))
				return ENOMEM;
		} else if (!jb_set(dst, it.key, copy(it.val))) {
			return ENOMEM;
		}
	}
	return 0;
}

struct json_object *
ai_request (struct ai_endpoint const *ep,
            struct json_object       *system,
            struct json_object       *prompt)
{
	struct json_object *root = json_object_new_object();
	if (!root)
		return nullptr;
	if (ep->model && ep->protocol != AI_GEMINI &&
	    !jb_set(root, "model", json_object_new_string(ep->model)))
		goto fail;

	struct json_object *options = root;
	char const *max_key = "max_tokens";
	long max_tokens = ep->max_tokens;
	switch (ep->protocol) {
	case AI_OPENAI: {
		struct json_object *messages = json_object_new_array();
		if (!jb_set(root, "messages", messages) ||
		    (system && !jb_push(messages, message("system", system))) ||
		    !jb_push(messages, message("user", prompt)))
			goto fail;
		break;
	}
	case AI_RESPONSES:
		max_key = "max_output_tokens";
		if (!jb_set(root, "input", json_object_get(prompt)) ||
		    (system && !jb_set(root, "instructions", json_object_get(system))) ||
		    !jb_set(root, "store", json_object_new_boolean(0)))
			goto fail;
		break;
	case AI_ANTHROPIC:
		if (max_tokens <= 0)
			max_tokens = ANTHROPIC_MAX_TOKENS;
		if (!jb_set(root, "messages", jb_list(message("user", prompt))) ||
		    (system && !jb_set(root, "system", json_object_get(system))))
			goto fail;
		break;
	case AI_GEMINI:
		max_key = "maxOutputTokens";
		if (!jb_set(root, "contents",
		            jb_list(gemini_content("user", prompt))) ||
		    (system && !jb_set(root, "systemInstruction",
		                       gemini_content(nullptr, system))))
			goto fail;
		if (max_tokens > 0 || ep->temperature) {
			options = json_object_new_object();
			if (!jb_set(root, "generationConfig", options))
				goto fail;
		}
		break;
	}

	/* Gemini streams by its URL; the others by the body, whatever
	 * --extra says. */
	if ((max_tokens > 0 &&
	     !jb_set(options, max_key, json_object_new_int64(max_tokens))) ||
	    (ep->temperature &&
	     !jb_set(options, "temperature", copy(ep->temperature))) ||
	    (ep->extra && merge(root, ep->extra)) ||
	    (ep->protocol != AI_GEMINI &&
	     !jb_set(root, "stream", json_object_new_boolean(1))))
		goto fail;
	return root;

fail:
	json_object_put(root);
	return nullptr;
}

static int
append (struct dstr        *text,
        struct json_object *val)
{
	char const *str = jb_text(val);
	return str && !dstr_cat_n(text, str, (size_t)json_object_get_string_len(val))
	       ? ENOMEM : 0;
}

static int
openai_answer (struct json_object *root,
               struct dstr        *text,
               struct answer      *answer)
{
	struct json_object *choice = jb_at(jb_get(root, "choices"), 0);
	struct json_object *msg = jb_get(choice, "message");
	struct json_object *content = jb_get(msg, "content");

	/* Some servers send content as an array of text parts. */
	int e = append(text, content);
	struct json_object *part;
	for (size_t i = 0; !e && (part = jb_at(content, i)); ++i)
		e = append(text, jb_get(part, "text"));
	if (!e && !dstr_len(text))
		e = append(text, jb_get(msg, "refusal"));

	answer->reason = jb_text(jb_get(choice, "finish_reason"));
	answer->truncated = jb_is(jb_get(choice, "finish_reason"), "length");
	return e;
}

static int
responses_answer (struct json_object *root,
                  struct dstr        *text,
                  struct answer      *answer)
{
	int e = 0;
	struct json_object *item;
	for (size_t i = 0; !e && (item = jb_at(jb_get(root, "output"), i)); ++i) {
		if (!jb_is(jb_get(item, "type"), "message"))
			continue;
		struct json_object *part;
		for (size_t j = 0; !e && (part = jb_at(jb_get(item, "content"), j)); ++j) {
			bool refusal = jb_is(jb_get(part, "type"), "refusal");
			e = append(text, jb_get(part, refusal ? "refusal" : "text"));
		}
	}

	struct json_object *reason = jb_get(jb_get(root, "incomplete_details"),
	                                    "reason");
	answer->reason = jb_text(reason);
	answer->truncated = jb_is(reason, "max_output_tokens");
	return e;
}

static int
anthropic_answer (struct json_object *root,
                  struct dstr        *text,
                  struct answer      *answer)
{
	int e = 0;
	struct json_object *block;
	for (size_t i = 0; !e && (block = jb_at(jb_get(root, "content"), i)); ++i)
		if (jb_is(jb_get(block, "type"), "text"))
			e = append(text, jb_get(block, "text"));

	answer->reason = jb_text(jb_get(root, "stop_reason"));
	answer->truncated = jb_is(jb_get(root, "stop_reason"), "max_tokens");
	return e;
}

static int
gemini_answer (struct json_object *root,
               struct dstr        *text,
               struct answer      *answer)
{
	struct json_object *candidate = jb_at(jb_get(root, "candidates"), 0);
	struct json_object *parts = jb_get(jb_get(candidate, "content"), "parts");
	int e = 0;
	struct json_object *part;
	for (size_t i = 0; !e && (part = jb_at(parts, i)); ++i)
		if (!json_object_get_boolean(jb_get(part, "thought")))
			e = append(text, jb_get(part, "text"));

	struct json_object *reason = candidate
		? jb_get(candidate, "finishReason")
		: jb_get(jb_get(root, "promptFeedback"), "blockReason");
	answer->reason = jb_text(reason);
	answer->truncated = jb_is(reason, "MAX_TOKENS");
	return e;
}

static char const no_answer[] = "The model returned no answer%s%s%s.";

int
ai_no_answer (char       *buf,
              size_t      size,
              char const *reason)
{
	return snprintf(buf, size, no_answer, reason ? " (stop reason: " : "",
	                reason ? reason : "", reason ? ")" : "");
}

/* Hand @p val's text to @p put, if it is a string. */
static int
put_text (int               (*put) (void       *user,
                                    char const *text,
                                    size_t      len),
          void               *user,
          struct json_object *val)
{
	char const *str = jb_text(val);
	return str ? put(user, str, (size_t)json_object_get_string_len(val)) : 0;
}

/* Whether @p val is a string with something in it. */
static bool
some_text (struct json_object *val)
{
	return jb_text(val) && json_object_get_string_len(val);
}

/* The message of an error event, or of one that carries an error. */
static char const *
event_error (struct json_object *ev)
{
	struct json_object *type = jb_get(ev, "type");
	struct json_object *error = jb_get(ev, "error");
	if (!error && jb_is(type, "response.failed"))
		error = jb_get(jb_get(ev, "response"), "error");
	bool erred = error || jb_is(type, "error");
	char const *msg = jb_text(jb_get(error, "message"));
	if (!msg)
		msg = jb_text(error);
	if (!msg && erred)
		msg = jb_text(jb_get(ev, "message"));
	return msg ? msg : erred ? "an error event" : nullptr;
}

int
ai_event (enum ai_protocol    protocol,
          struct json_object *ev,
          int               (*put) (void       *user,
                                    char const *text,
                                    size_t      len),
          void               *user,
          struct ai_event    *out)
{
	if ((out->error = event_error(ev)))
		return 0;
	int e = 0;
	struct json_object *type = jb_get(ev, "type");
	struct json_object *reason = nullptr;
	char const *limit = nullptr;
	switch (protocol) {
	case AI_OPENAI: {
		/* The first completion's, should --extra ask for more (n). */
		struct json_object *choices = jb_get(ev, "choices");
		struct json_object *choice;
		for (size_t i = 0; (choice = jb_at(choices, i)) &&
		                   json_object_get_int64(jb_get(choice, "index")); ++i)
			;
		struct json_object *delta = jb_get(choice, "delta");
		struct json_object *content = jb_get(delta, "content");
		/* Some servers send content as an array of text parts. */
		e = put_text(put, user, content);
		struct json_object *part;
		for (size_t i = 0; !e && (part = jb_at(content, i)); ++i)
			e = put_text(put, user, jb_get(part, "text"));
		if (!e)
			e = put_text(put, user, jb_get(delta, "refusal"));
		/* llama-server's and DeepSeek's name, and OpenRouter's. */
		out->thought = some_text(jb_get(delta, "reasoning_content")) ||
		               some_text(jb_get(delta, "reasoning"));
		reason = jb_get(choice, "finish_reason");
		limit = "length";
		break;
	}
	case AI_RESPONSES:
		if (jb_is(type, "response.output_text.delta") ||
		    jb_is(type, "response.refusal.delta"))
			e = put_text(put, user, jb_get(ev, "delta"));
		out->thought = (jb_is(type, "response.reasoning_text.delta") ||
		                jb_is(type, "response.reasoning_summary_text.delta")) &&
		               some_text(jb_get(ev, "delta"));
		reason = jb_get(jb_get(jb_get(ev, "response"), "incomplete_details"),
		                "reason");
		limit = "max_output_tokens";
		break;
	case AI_ANTHROPIC: {
		struct json_object *delta = jb_get(ev, "delta");
		if (jb_is(type, "content_block_delta") &&
		    jb_is(jb_get(delta, "type"), "text_delta"))
			e = put_text(put, user, jb_get(delta, "text"));
		out->thought = jb_is(type, "content_block_delta") &&
		               jb_is(jb_get(delta, "type"), "thinking_delta") &&
		               some_text(jb_get(delta, "thinking"));
		reason = jb_get(delta, "stop_reason");
		limit = "max_tokens";
		break;
	}
	case AI_GEMINI: {
		struct json_object *candidate = jb_at(jb_get(ev, "candidates"), 0);
		struct json_object *parts = jb_get(jb_get(candidate, "content"), "parts");
		struct json_object *part;
		for (size_t i = 0; !e && (part = jb_at(parts, i)); ++i)
			if (!json_object_get_boolean(jb_get(part, "thought")))
				e = put_text(put, user, jb_get(part, "text"));
			else
				out->thought |= some_text(jb_get(part, "text"));
		reason = candidate ? jb_get(candidate, "finishReason")
		       : jb_get(jb_get(ev, "promptFeedback"), "blockReason");
		limit = "MAX_TOKENS";
		break;
	}
	}
	if ((out->reason = jb_text(reason)))
		out->truncated = !strcmp(out->reason, limit);
	out->end = out->reason || jb_is(type, "response.completed") ||
	           jb_is(type, "response.incomplete") || jb_is(type, "message_stop");
	return e;
}

static int
describe_error (struct dstr        *text,
                long                status,
                struct json_object *root,
                char const         *body,
                size_t              len)
{
	struct json_object *error = jb_get(root, "error");
	char const *msg = jb_text(jb_get(error, "message"));
	if (!msg)
		msg = jb_text(error);
	if (!msg)
		msg = jb_text(jb_get(root, "message"));
	if (!msg)
		msg = jb_text(jb_get(root, "detail"));

	if (!dstr_printf(text, "Upstream error (HTTP %ld): ", status))
		return ENOMEM;
	if (msg)
		return dstr_cat(text, msg) ? 0 : ENOMEM;

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
		return describe_error(text, status, root, body, len);

	struct answer answer = {0};
	int e = 0;
	switch (protocol) {
	case AI_OPENAI:
		e = openai_answer(root, text, &answer);
		break;
	case AI_RESPONSES:
		e = responses_answer(root, text, &answer);
		break;
	case AI_ANTHROPIC:
		e = anthropic_answer(root, text, &answer);
		break;
	case AI_GEMINI:
		e = gemini_answer(root, text, &answer);
		break;
	}

	if (!e && !dstr_len(text)) {
		*failed = true;
		e = dstr_printf(text, no_answer, answer.reason ? " (stop reason: " : "",
		                answer.reason ? answer.reason : "",
		                answer.reason ? ")" : "") ? 0 : ENOMEM;
	} else if (!e && answer.truncated) {
		e = dstr_cat_n(text, AI_CUT_NOTE, sizeof AI_CUT_NOTE - 1) ? 0 : ENOMEM;
	}
	return e;
}
