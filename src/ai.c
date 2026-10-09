/** @file Upstream AI endpoint protocols: request bodies and answer parsing. */
#include <errno.h>

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
message (char const *role,
         char const *content)
{
	return jb_object("role", json_object_new_string(role),
	                 "content", json_object_new_string(content),
	                 nullptr);
}

static struct json_object *
gemini_content (char const *role,
                char const *text)
{
	struct json_object *parts = jb_list(jb_object(
		"text", json_object_new_string(text), nullptr));
	return role ? jb_object("role", json_object_new_string(role),
	                        "parts", parts, nullptr)
	            : jb_object("parts", parts, nullptr);
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
		} else if (!jb_set(dst, it.key, json_object_get(it.val))) {
			return ENOMEM;
		}
	}
	return 0;
}

struct json_object *
ai_request (struct ai_endpoint const *ep,
            char const               *system,
            char const               *prompt)
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
		if (!jb_set(root, "input", json_object_new_string(prompt)) ||
		    (system && !jb_set(root, "instructions",
		                       json_object_new_string(system))) ||
		    !jb_set(root, "store", json_object_new_boolean(0)))
			goto fail;
		break;
	case AI_ANTHROPIC:
		if (max_tokens <= 0)
			max_tokens = ANTHROPIC_MAX_TOKENS;
		if (!jb_set(root, "messages", jb_list(message("user", prompt))) ||
		    (system && !jb_set(root, "system", json_object_new_string(system))))
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

	if ((max_tokens > 0 &&
	     !jb_set(options, max_key, json_object_new_int64(max_tokens))) ||
	    (ep->temperature &&
	     !jb_set(options, "temperature", json_object_get(ep->temperature))) ||
	    (ep->extra && merge(root, ep->extra)))
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

	/* Quote the start of an unparsable body, not splitting a UTF-8 sequence. */
	size_t cut = len < 512 ? len : 512;
	while (cut < len && cut && ((unsigned char)body[cut] & 0xC0) == 0x80)
		--cut;
	return dstr_cat_n(text, cut ? body : "empty response",
	                  cut ? cut : sizeof "empty response" - 1) ? 0 : ENOMEM;
}

int
ai_response (enum ai_protocol  protocol,
             long              status,
             char const       *body,
             size_t            len,
             struct dstr      *text,
             bool             *failed)
{
	static char const cut_note[] =
		"\n\n[The answer was cut short by the output token limit.]";

	struct json_object *root = jb_parse(body, len, 0);
	*failed = status < 200 || status > 299 || !root || jb_get(root, "error");
	if (*failed) {
		int e = describe_error(text, status, root, body, len);
		json_object_put(root);
		return e;
	}

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
		e = dstr_printf(text, "The model returned no answer%s%s%s.",
		                answer.reason ? " (stop reason: " : "",
		                answer.reason ? answer.reason : "",
		                answer.reason ? ")" : "") ? 0 : ENOMEM;
	} else if (!e && answer.truncated) {
		e = dstr_cat_n(text, cut_note, sizeof cut_note - 1) ? 0 : ENOMEM;
	}
	json_object_put(root);
	return e;
}
