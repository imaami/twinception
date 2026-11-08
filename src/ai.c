/** @file ai.c
 *
 * Upstream AI APIs: request bodies and answer parsing, whole or streamed.
 */
#include <errno.h>
#include <stdckdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "ai.h"
#include "dstr.h"
#include "jb.h"
#include "util.h"

/** Anthropic requires max_tokens; this fits every model's output limit. */
enum { MESSAGES_MAX_TOKENS = 4096 };

/* The state of the reading of Gemini's Interactions (struct ai_reader's
 * state): in INTERACTIONS_KIND, the type of the open step, or the end of
 * the answer, after which no event changes the replay; then the state of
 * the summary array of the open thought, and its signature. An open
 * thought is not in the replay until it has a summary item or a signature.
 */
enum {
	INTERACTIONS_THOUGHT = 0,
	INTERACTIONS_OUTPUT  = 1,
	INTERACTIONS_ENDED   = 2,
	INTERACTIONS_KIND    = 3,
	SUMMARY_OPEN         = 1u << 2,
	SUMMARY_CLOSED       = 1u << 3,
	SIGNED               = 1u << 4,
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
		char const *role = jb_view(jb_get(message, "role")).data;
		messages = jb_push_or_free(messages, ai_input(api, role, jb_get(message, "content")));
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
		bool deep = json_object_is_type(it.val, json_type_object) &&
		            json_object_is_type(cur, json_type_object);
		if (!it.val)
			json_object_object_del(dst, it.key);
		else if (deep ? ai_merge(cur, it.val) : jb_set(dst, it.key, ai_copy(it.val)))
			return ENOMEM;
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

/* Append the text of @p val to @p text if @p val is a string and @p text
 * is not nullptr.
 */
static int
ai_append (struct dstr        *text,
           struct json_object *val)
{
	struct cstr_view str = jb_view(val);
	return text && str.data && !dstr_cat_n(text, str.data, str.len) ? ENOMEM : 0;
}

/* Append the text of each of @p parts, an array of objects with text, as
 * ai_append() does.
 */
static int
ai_append_parts (struct dstr        *text,
                 struct json_object *parts)
{
	int e = 0;
	struct json_object *part;
	for (size_t i = 0; !e && (part = jb_at(parts, i)); ++i)
		e = ai_append(text, jb_get(part, "text"));
	return e;
}

/* The thinking in @p message, a message or a delta of chat completions:
 * reasoning_content, as llama-server and DeepSeek send it, unless it is
 * empty, else reasoning, as OpenRouter sends it.
 */
static struct json_object *
ai_chat_reasoning (struct json_object *message)
{
	struct json_object *thinking = jb_get(message, "reasoning_content");
	return jb_view(thinking).len ? thinking : jb_get(message, "reasoning");
}

/* These functions read a whole answer @p root of their API: they append
 * its text to @p text and its thinking to @p thought, each unless it is
 * nullptr, and store its stop reason in @p *reason.
 */

static int
ai_chat_answer (struct json_object  *root,
                struct dstr         *text,
                struct dstr         *thought,
                char const         **reason)
{
	struct json_object *choice = jb_at(jb_get(root, "choices"), 0);
	struct json_object *msg = jb_get(choice, "message");
	struct json_object *content = jb_get(msg, "content");
	*reason = jb_view(jb_get(choice, "finish_reason")).data;

	/* Some servers send content as an array of text parts. */
	int e = ai_append(thought, ai_chat_reasoning(msg));
	e = e ? e : ai_append(text, content);
	e = e ? e : ai_append_parts(text, content);
	if (e || !text || dstr_len(text))
		return e;
	return ai_append(text, jb_get(msg, "refusal"));
}

static int
ai_responses_answer (struct json_object  *root,
                     struct dstr         *text,
                     struct dstr         *thought,
                     char const         **reason)
{
	*reason = jb_view(jb_get(jb_get(root, "incomplete_details"), "reason")).data;
	struct json_object *output = jb_get(root, "output");
	int e = 0;
	struct json_object *item;
	for (size_t i = 0; !e && (item = jb_at(output, i)); ++i) {
		struct json_object *type = jb_get(item, "type");
		if (jb_is(type, "reasoning")) {
			e = ai_append_parts(thought, jb_get(item, "content"));
			e = e ? e : ai_append_parts(thought, jb_get(item, "summary"));
			continue;
		}
		if (!jb_is(type, "message"))
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
                    struct dstr         *thought,
                    char const         **reason)
{
	*reason = jb_view(jb_get(root, "stop_reason")).data;
	struct json_object *content = jb_get(root, "content");
	int e = 0;
	struct json_object *block;
	for (size_t i = 0; !e && (block = jb_at(content, i)); ++i) {
		struct json_object *type = jb_get(block, "type");
		e = jb_is(type, "text") ? ai_append(text, jb_get(block, "text"))
		  : jb_is(type, "thinking") ? ai_append(thought, jb_get(block, "thinking"))
		  : 0;
	}
	return e;
}

static int
ai_generate_answer (struct json_object  *root,
                    struct dstr         *text,
                    struct dstr         *thought,
                    char const         **reason)
{
	struct json_object *candidate = jb_at(jb_get(root, "candidates"), 0);
	struct json_object *parts = jb_get(jb_get(candidate, "content"), "parts");
	*reason = jb_view(candidate ? jb_get(candidate, "finishReason")
	                            : jb_get(jb_get(root, "promptFeedback"), "blockReason")).data;
	int e = 0;
	struct json_object *part;
	for (size_t i = 0; !e && (part = jb_at(parts, i)); ++i)
		e = ai_append(jb_true(jb_get(part, "thought")) ? thought : text,
		              jb_get(part, "text"));
	return e;
}

/* Gemini's Interactions has the text in its model_output steps, the
 * thinking in its thought steps, and its status as its stop reason.
 */
static int
ai_interactions_answer (struct json_object  *root,
                        struct dstr         *text,
                        struct dstr         *thought,
                        char const         **reason)
{
	*reason = jb_view(jb_get(root, "status")).data;
	struct json_object *steps = jb_get(root, "steps");
	int e = 0;
	struct json_object *step;
	for (size_t i = 0; !e && (step = jb_at(steps, i)); ++i) {
		struct json_object *type = jb_get(step, "type");
		if (jb_is(type, "thought")) {
			e = ai_append_parts(thought, jb_get(step, "summary"));
			continue;
		}
		if (!jb_is(type, "model_output"))
			continue;
		struct json_object *content = jb_get(step, "content");
		struct json_object *item;
		for (size_t j = 0; !e && (item = jb_at(content, j)); ++j)
			e = jb_is(jb_get(item, "type"), "text") ? ai_append(text, jb_get(item, "text")) : 0;
	}
	return e;
}

/* What each API has: the format of its turns, how its body is built
 * around its conversation, the member that the format names, and how its
 * answers and tools are read and written. The body has the member of the
 * system prompt, which chat completions has in the conversation instead;
 * the object of the generation options, or the body itself; the member of
 * the token limit, and its default; and the other members that the flags
 * name.
 */
enum {
	BODY_MODEL   = 1 << 0,  /* the model, first */
	BODY_USAGE   = 1 << 1,  /* with tools, the usage in the stream */
	BODY_STORE   = 1 << 2,  /* "store", false */
	BODY_KEEP    = 1 << 3,  /* "store", true with native background
	                           execution, which needs storage */
	BODY_STREAM  = 1 << 4,  /* "stream", after --extra is merged, so that
	                           the answer is always streamed */
	BODY_PARTS   = 1 << 5,  /* the system prompt in an object with an
	                           array of parts */
	TOOL_TYPED   = 1 << 6,  /* a tool has "type": "function" */
	TOOL_WRAPPED = 1 << 7,  /* a tool is the member function of an object
	                           with "type": "function" */
};

#define B(s) CSTR_VIEW_INIT(s)
static struct ai_body {
	struct ai_format format;
	char const      *system;
	char const      *options;
	char const      *max;
	char const      *limit;        /* the stop reason of an answer that the
	                                  output token limit cut */
	char const      *schema;       /* the member of a tool's input schema */
	int            (*answer) (struct json_object  *root,
	                          struct dstr         *text,
	                          struct dstr         *thought,
	                          char const         **reason);
	int32_t          max_default;
	uint32_t         flags;
} const ai_bodies[] = {
	[AI_CHAT] = {
		.format = {
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
		.max    = "max_tokens",
		.limit  = "length",
		.schema = "parameters",
		.answer = ai_chat_answer,
		.flags  = BODY_MODEL | BODY_USAGE | BODY_STREAM | TOOL_WRAPPED,
	},
	/* Its turn is the replay's items, its calls among them, in the
	 * input's list. */
	[AI_RESPONSES] = {
		.format = {
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
		.system = "instructions",
		.max    = "max_output_tokens",
		.limit  = "max_output_tokens",
		.schema = "parameters",
		.answer = ai_responses_answer,
		.flags  = BODY_MODEL | BODY_STORE | BODY_STREAM | TOOL_TYPED,
	},
	/* Its turn's text and thinking blocks are in the replay; all its
	 * results go in one message. */
	[AI_MESSAGES] = {
		.format = {
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
		.system      = "system",
		.max         = "max_tokens",
		.max_default = MESSAGES_MAX_TOKENS,
		.limit       = "max_tokens",
		.schema      = "input_schema",
		.answer      = ai_messages_answer,
		.flags       = BODY_MODEL | BODY_STREAM,
	},
	/* Its turn's parts are the replay's, its calls among them, with
	 * their thought signatures. A file goes after the results, in
	 * their message, as the model does not see a text file in a
	 * result. */
	[AI_GENERATE] = {
		.format = {
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
		.system  = "systemInstruction",
		.options = "generationConfig",
		.max     = "maxOutputTokens",
		.limit   = "MAX_TOKENS",
		.schema  = "parametersJsonSchema",
		.answer  = ai_generate_answer,
		.flags   = BODY_PARTS,
	},
	/* A turn is the replay's steps and the calls, in the input list,
	 * a call with its signature. Each result is a step after them,
	 * which the API refuses without the call's name. A file is a
	 * user_input step after the results, as a result takes no
	 * document. */
	[AI_INTERACTIONS] = {
		.format = {
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
		.system  = "system_instruction",
		.options = "generation_config",
		.max     = "max_output_tokens",
		.limit   = "incomplete",
		.schema  = "parameters",
		.answer  = ai_interactions_answer,
		.flags   = BODY_MODEL | BODY_STORE | BODY_KEEP | BODY_STREAM | TOOL_TYPED,
	},
};
#undef B

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
	char const *conversation = b->format.conversation;
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

size_t
ai_no_answer (char       *buf,
              size_t      size,
              char const *reason)
{
	return reason ? util_format(buf, size, "The model returned no answer (stop reason: %s).",
	                            reason)
	              : util_format(buf, size, "The model returned no answer.");
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
 * unless sink->thought or text.data is nullptr. Set AI_EV_THOUGHT if
 * @p text is not empty.
 */
static int
ai_put_thought (struct ai_sink const *sink,
                struct cstr_view      text,
                struct ai_event      *out)
{
	out->flags |= text.len ? AI_EV_THOUGHT : 0;
	return text.data && sink->thought ? sink->thought(sink->user, text) : 0;
}

/* Return @p val, the index of a call or of a content block, or
 * AI_CALL_NEXT if @p val is not such an index.
 */
static uint32_t
ai_index (struct json_object *val)
{
	int64_t at = jb_int(val, -1);
	return at >= 0 && at < AI_CALL_NEXT ? (uint32_t)at : AI_CALL_NEXT;
}

/* Return the sum of @p prompt, the token count of a request, and @p answer,
 * the token count of its answer. Return UINT32_MAX if the sum is larger
 * than UINT32_MAX. Return 0, which means that the count is not known, if
 * @p prompt is not positive, if @p answer is negative, or if the sum
 * overflows int64_t. The callers read a count that is not a JSON integer
 * as 0 with jb_int().
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
	struct cstr_view const json = jb_wire(val);
	return json.data ? sink->replay(sink->user, cstr_view_slice(json, 1, json.len - 2))
	                 : ENOMEM;
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
	struct cstr_view const json = jb_wire(val);
	int e = val && !json.data ? ENOMEM : sink->replay(sink->user, head);
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
		/* extra_content contains the thought signature of Gemini, which
		 * askai sends back unchanged in the next turn. */
		struct json_object *extra = jb_get(c, "extra_content");
		struct ai_call call = {
			.id    = jb_view(jb_get(c, "id")),
			.name  = jb_view(jb_get(fn, "name")),
			.args  = jb_view(jb_get(fn, "arguments")),
			.extra = jb_wire(extra),
			.index = ai_index(jb_get(c, "index")),
		};
		e = extra && !call.extra.data ? ENOMEM : sink->call(sink->user, &call);
	}
	return e;
}

/* The message of @p error, an upstream's error member: its message, or
 * itself if it is a string; nullptr if neither is a string.
 */
static char const *
ai_error_text (struct json_object *error)
{
	char const *msg = jb_view(jb_get(error, "message")).data;
	return msg ? msg : jb_view(error).data;
}

/* The message of an error event, or of one that carries an error. */
static char const *
ai_event_error (struct json_object *ev)
{
	struct json_object *type = jb_get(ev, "type");
	struct json_object *error = jb_get(ev, "error");
	if (!error && jb_is(type, "response.failed"))
		error = jb_get(jb_get(ev, "response"), "error");
	char const *msg = ai_error_text(error);
	/* An event of Gemini's Interactions has its type in event_type. */
	if (msg || !(error || jb_is(type, "error") || jb_is(jb_get(ev, "event_type"), "error")))
		return msg;
	msg = jb_view(jb_get(ev, "message")).data;
	return msg ? msg : "an error event";
}

bool
ai_cut (enum ai_api       api,
        char const       *reason)
{
	return reason && !strcmp(reason, ai_bodies[api].limit);
}

/* Pass the pieces of @p delta of chat completions to @p sink. Set
 * AI_EV_THOUGHT if the delta contains thinking.
 */
static int
ai_chat_delta (struct ai_sink const *sink,
               struct json_object   *delta,
               struct ai_event      *out)
{
	/* Thinking precedes visible text when a provider includes both in one
	 * delta. */
	int e = ai_put_thought(sink, jb_view(ai_chat_reasoning(delta)), out);
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
 * AI_EV_THOUGHT_END if @p item is a reasoning item.
 */
static int
ai_responses_item (struct ai_sink const *sink,
                   struct json_object   *at,
                   struct json_object   *item,
                   struct ai_event      *out)
{
	enum { CALL, REASONING, MESSAGE, KINDS };
	static struct cstr_view const kinds[KINDS] = {
		[CALL]      = CSTR_VIEW_INIT("function_call"),
		[REASONING] = CSTR_VIEW_INIT("reasoning"),
		[MESSAGE]   = CSTR_VIEW_INIT("message"),
	};
	size_t kind = cstr_view_index(kinds, KINDS, jb_view(jb_get(item, "type")));
	if (kind == KINDS)
		return 0;
	out->flags |= kind == REASONING ? AI_EV_THOUGHT_END : 0;
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
 * AI_EV_THOUGHT if the event contains thinking. Set AI_EV_END and
 * out->used when the answer ends.
 */
static int
ai_responses_event (struct ai_sink const *sink,
                    struct json_object   *ev,
                    struct ai_event      *out)
{
	enum { TEXT, REFUSAL, REASONING, SUMMARY, ITEM, DONE, CUT, KINDS };
	static struct cstr_view const kinds[KINDS] = {
		[TEXT]      = CSTR_VIEW_INIT("response.output_text.delta"),
		[REFUSAL]   = CSTR_VIEW_INIT("response.refusal.delta"),
		[REASONING] = CSTR_VIEW_INIT("response.reasoning_text.delta"),
		[SUMMARY]   = CSTR_VIEW_INIT("response.reasoning_summary_text.delta"),
		[ITEM]      = CSTR_VIEW_INIT("response.output_item.done"),
		[DONE]      = CSTR_VIEW_INIT("response.completed"),
		[CUT]       = CSTR_VIEW_INIT("response.incomplete"),
	};
	struct json_object *delta = jb_get(ev, "delta");
	switch (cstr_view_index(kinds, KINDS, jb_view(jb_get(ev, "type")))) {
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
		out->flags |= AI_EV_END;
		out->used = ai_used(jb_int(jb_get(usage, "input_tokens"), 0),
		                    jb_int(jb_get(usage, "output_tokens"), 0));
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
	bool whole = jb_members(args);
	struct ai_call call = {
		.id    = jb_view(jb_get(item, "id")),
		.name  = jb_view(jb_get(item, "name")),
		.args  = jb_wire(whole ? args : nullptr),
		.extra = jb_wire(extra),
		.index = at,
	};
	if ((whole && !call.args.data) || (extra && !call.extra.data))
		return ENOMEM;
	return sink->call(sink->user, &call);
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
	static struct cstr_view const kinds[KINDS] = {
		[TOOL]     = CSTR_VIEW_INIT("tool_use"),
		[THINKING] = CSTR_VIEW_INIT("thinking"),
		[REDACTED] = CSTR_VIEW_INIT("redacted_thinking"),
	};
	switch (cstr_view_index(kinds, KINDS, jb_view(jb_get(block, "type")))) {
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
 * block in a turn. Set AI_EV_THOUGHT if the delta contains thinking.
 */
static int
ai_messages_delta (struct ai_sink const *sink,
                   struct ai_reader     *reader,
                   uint32_t              at,
                   struct json_object   *delta,
                   struct ai_event      *out)
{
	enum { TEXT, THINKING, SIGNATURE, INPUT, KINDS };
	static struct cstr_view const kinds[KINDS] = {
		[TEXT]      = CSTR_VIEW_INIT("text_delta"),
		[THINKING]  = CSTR_VIEW_INIT("thinking_delta"),
		[SIGNATURE] = CSTR_VIEW_INIT("signature_delta"),
		[INPUT]     = CSTR_VIEW_INIT("input_json_delta"),
	};
	bool open = reader->open == at + 1;
	switch (cstr_view_index(kinds, KINDS, jb_view(jb_get(delta, "type")))) {
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
 * with the state of the reading in @p reader. Set AI_EV_THOUGHT if the
 * event contains thinking, out->used when the answer ends, and AI_EV_END
 * at the end of the message. An event of a content block without an index
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
	static struct cstr_view const kinds[KINDS] = {
		[DELTA]         = CSTR_VIEW_INIT("content_block_delta"),
		[START]         = CSTR_VIEW_INIT("content_block_start"),
		[STOP]          = CSTR_VIEW_INIT("content_block_stop"),
		[MESSAGE]       = CSTR_VIEW_INIT("message_start"),
		[MESSAGE_DELTA] = CSTR_VIEW_INIT("message_delta"),
		[MESSAGE_STOP]  = CSTR_VIEW_INIT("message_stop"),
	};
	uint32_t at = ai_index(jb_get(ev, "index"));
	size_t kind = cstr_view_index(kinds, KINDS, jb_view(jb_get(ev, "type")));
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
		out->flags |= reader->calls == at + 1 ? AI_EV_THOUGHT_END : 0;
		reader->calls = 0;
		reader->open = 0;
		return ai_replay(sink, CSTR_VIEW("\"}"));
	case MESSAGE: {
		struct json_object *usage = jb_get(jb_get(ev, "message"), "usage");
		struct json_object *written = jb_get(usage, "cache_creation_input_tokens");
		struct json_object *cached = jb_get(usage, "cache_read_input_tokens");
		int64_t n = jb_int(jb_get(usage, "input_tokens"), 0);
		/* A sum that overflows int64_t is not a token count. */
		bool over = ckd_add(&n, n, jb_int(written, 0)) ||
		            ckd_add(&n, n, jb_int(cached, 0));
		reader->input = over ? 0 : ai_used(n, 0);
		return 0;
	}
	case MESSAGE_DELTA: {
		struct json_object *usage = jb_get(ev, "usage");
		out->used = ai_used(reader->input,
		                    jb_int(jb_get(usage, "output_tokens"), 0));
		return 0;
	}
	case MESSAGE_STOP:
		out->flags |= AI_EV_END;
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
 * the replay. Thinking goes to sink->thought and sets AI_EV_THOUGHT. Other
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
		e = e ? e : ai_call_start(sink, reader->calls++, fc, "args", nullptr);
		return e ? e : ai_replay_item(sink, part);
	}
	struct json_object *text = jb_get(part, "text");
	struct cstr_view piece = jb_view(text);
	bool const thought = jb_true(jb_get(part, "thought"));
	int e = thought ? ai_put_thought(sink, piece, out) : ai_put_text(sink, piece);
	if (e)
		return e;
	if (jb_get(part, "thoughtSignature"))
		return ai_generate_whole(sink, reader, part);
	if (thought || !piece.len)
		return 0;
	if (reader->open)
		return ai_replay_text(sink, text);
	reader->open = 1;
	return ai_replay_open(sink, CSTR_VIEW(",{\"text\":\""), text);
}

/* Write the start of the open thought of @p reader into the replay of
 * @p sink, unless the start is in the replay already. The start is in the
 * replay once a summary item or the signature of the thought is written.
 * The API rejects a thought without a summary or a signature.
 */
static int
ai_interactions_head (struct ai_sink const   *sink,
                      struct ai_reader const *reader)
{
	if (reader->state & (SUMMARY_OPEN | SUMMARY_CLOSED | SIGNED))
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
	switch (reader->state) {
	case INTERACTIONS_OUTPUT:
		return ai_replay(sink, CSTR_VIEW("\"}]}"));
	case INTERACTIONS_THOUGHT:
		return 0;
	case INTERACTIONS_THOUGHT | SUMMARY_OPEN:
	case INTERACTIONS_THOUGHT | SUMMARY_OPEN | SIGNED:
		return ai_replay(sink, CSTR_VIEW("]}"));
	default:
		return ai_replay(sink, CSTR_VIEW("}"));
	}
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
	int e = ai_put_text(sink, piece);
	if (e)
		return e;
	if (reader->open == at + 1)
		return reader->state == INTERACTIONS_OUTPUT ? ai_replay_text(sink, text) : 0;
	if (reader->open || !cstr_view_trim_end(piece).len)
		return 0;
	reader->open = at + 1;
	reader->state = INTERACTIONS_OUTPUT;
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
	if (reader->state & SUMMARY_CLOSED || !json_object_is_type(item, json_type_object))
		return 0;
	if (reader->state & SUMMARY_OPEN)
		return ai_replay_item(sink, item);
	int e = ai_interactions_head(sink, reader);
	reader->state |= SUMMARY_OPEN;
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
	if (reader->state & SIGNED || !jb_view(signature).len)
		return 0;
	int e = ai_interactions_head(sink, reader);
	reader->state |= SIGNED;
	if (!e && reader->state & SUMMARY_OPEN) {
		reader->state ^= SUMMARY_OPEN | SUMMARY_CLOSED;
		e = ai_replay(sink, CSTR_VIEW("]"));
	}
	e = e ? e : ai_replay_open(sink, CSTR_VIEW(",\"signature\":\""), signature);
	return e ? e : ai_replay(sink, CSTR_VIEW("\""));
}

/* Mark @p step, the thought step @p at of Gemini's Interactions, open in
 * @p reader, and write the signature and the summary items in the start of
 * the step into the replay of @p sink. The thought is written to the
 * replay with its first summary item or its signature. Set AI_EV_THOUGHT
 * if the text of a summary item is not empty.
 */
static int
ai_interactions_thought (struct ai_sink const *sink,
                         struct ai_reader     *reader,
                         uint32_t              at,
                         struct json_object   *step,
                         struct ai_event      *out)
{
	reader->open = at + 1;
	reader->state = INTERACTIONS_THOUGHT;
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
	static struct cstr_view const kinds[KINDS] = {
		[THOUGHT] = CSTR_VIEW_INIT("thought"),
		[OUTPUT]  = CSTR_VIEW_INIT("model_output"),
		[CALL]    = CSTR_VIEW_INIT("function_call"),
	};
	size_t kind = cstr_view_index(kinds, KINDS, jb_view(jb_get(step, "type")));
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
		                     jb_view(signature).len ? signature : nullptr);
	}
	}
	return 0;
}

/* Process @p delta, a delta of step @p at of Gemini's Interactions. Pass
 * the text of a model output (ai_interactions_text()). Write a summary
 * item or a signature into the thought of step @p at if that thought is
 * open, and set AI_EV_THOUGHT if the text of the summary item is not
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
	static struct cstr_view const kinds[KINDS] = {
		[TEXT]      = CSTR_VIEW_INIT("text"),
		[SUMMARY]   = CSTR_VIEW_INIT("thought_summary"),
		[SIGNATURE] = CSTR_VIEW_INIT("thought_signature"),
		[ARGUMENTS] = CSTR_VIEW_INIT("arguments_delta"),
	};
	bool thought = reader->open == at + 1 &&
	               (reader->state & INTERACTIONS_KIND) == INTERACTIONS_THOUGHT;
	switch (cstr_view_index(kinds, KINDS, jb_view(jb_get(delta, "type")))) {
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
 * open item in the replay, sets AI_EV_END and the tokens used, and makes
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
	static struct cstr_view const kinds[KINDS] = {
		[DELTA] = CSTR_VIEW_INIT("step.delta"),
		[START] = CSTR_VIEW_INIT("step.start"),
		[STOP]  = CSTR_VIEW_INIT("step.stop"),
		[DONE]  = CSTR_VIEW_INIT("interaction.completed"),
	};
	if (reader->state == INTERACTIONS_ENDED)
		return 0;
	uint32_t at = ai_index(jb_get(ev, "index"));
	size_t kind = cstr_view_index(kinds, KINDS, jb_view(type));
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
		out->flags |= (reader->state & INTERACTIONS_KIND) == INTERACTIONS_THOUGHT
		              ? AI_EV_THOUGHT_END : 0;
		return ai_interactions_close(sink, reader);
	case DONE: {
		/* The thinking is not sent back in the next request, so the tokens
		 * used are total_input_tokens plus total_output_tokens, which leave
		 * out the thinking. If that sum is not known, the tokens used are
		 * total_tokens, which include the thinking. */
		struct json_object *usage = jb_get(jb_get(ev, "interaction"), "usage");
		out->flags |= AI_EV_END;
		out->used = ai_used(jb_int(jb_get(usage, "total_input_tokens"), 0),
		                    jb_int(jb_get(usage, "total_output_tokens"), 0));
		if (!out->used)
			out->used = ai_used(jb_int(jb_get(usage, "total_tokens"), 0), 0);
		int e = ai_interactions_close(sink, reader);
		reader->state = INTERACTIONS_ENDED;
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
		                   jb_int(jb_get(choice, "index"), 0); ++i)
			;
		/* With include_usage, a last event of its own, with no choices. */
		struct json_object *usage = jb_get(ev, "usage");
		out->used = ai_used(jb_int(jb_get(usage, "prompt_tokens"), 0),
		                    jb_int(jb_get(usage, "completion_tokens"), 0));
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
		out->used = ai_used(jb_int(jb_get(usage, "totalTokenCount"), 0), 0);
		break;
	}
	case AI_INTERACTIONS:
		/* interaction.created has a status too, so only the status in
		 * interaction.completed is the stop reason. */
		e = ai_interactions_event(sink, reader, jb_get(ev, "event_type"), ev, out);
		reason = out->flags & AI_EV_END ? jb_get(jb_get(ev, "interaction"), "status")
		                                : nullptr;
		break;
	case AI_NO_API:
		break;
	}
	out->reason = jb_view(reason).data;
	if (out->reason)
		out->flags |= AI_EV_END | (ai_cut(api, out->reason) ? AI_EV_TRUNCATED : 0);
	return e;
}

struct ai_format const *
ai_format (enum ai_api api)
{
	return &ai_bodies[api].format;
}

struct json_object *
ai_tool (enum ai_api               api,
         struct json_object const *def)
{
	struct ai_body const *b = &ai_bodies[api];
	struct json_object *tool = jb_object(
		"name", json_object_get(jb_get(def, "name")),
		"description", json_object_get(jb_get(def, "description")),
		b->schema, json_object_get(jb_get(def, "inputSchema")),
		nullptr);
	return b->flags & TOOL_WRAPPED
	       ? jb_object("type", json_object_new_string("function"), "function", tool, nullptr)
	       : b->flags & TOOL_TYPED
	       ? jb_set_or_free(tool, "type", json_object_new_string("function"))
	       : tool;
}

/* The data of the first event of the event stream @p body, parsed, if it
 * is JSON text on a data line that ends in the first
 * 4 KiB; else nullptr. Gemini's Interactions sends the error of a streamed
 * request as such an event, with a status that is not 2xx, so askai gathers
 * the body whole and cannot parse it as JSON text.
 */
static struct json_object *
ai_error_event (struct cstr_view body)
{
	struct cstr_view rest = cstr_view_slice(body, 0, 4096);
	for (struct cstr_view line; cstr_view_cut(&rest, '\n', &line);) {
		struct cstr_view const data = cstr_view_sse_data(&line);
		if (data.data)
			return jb_parse(data.data, data.len, JSON_TOKENER_STRICT, 8);
	}
	return nullptr;
}

/* Appends the error in the answer @p body of HTTP @p status to @p text.
 * @p root is the parsed body. The error is the message of the answer, or
 * the message of its first event, or the start of @p body.
 */
static int
ai_describe_error (struct dstr        *text,
                   long                status,
                   struct json_object *root,
                   struct cstr_view    body)
{
	if (!dstr_printf(text, "Upstream error (HTTP %ld): ", status))
		return ENOMEM;
	struct json_object *event = root ? nullptr : ai_error_event(body);
	char const *said = event ? ai_event_error(event) : ai_error_text(jb_get(root, "error"));
	said = said ? said : jb_view(jb_get(root, "message")).data;
	said = said ? said : jb_view(jb_get(root, "detail")).data;
	/* Else quote the start of an unparsable body, as far as it is UTF-8
	 * text. */
	size_t cut = said ? 0 : jb_utf8(body.data, body.len < 512 ? body.len : 512);
#define QUOTE(s) dstr_cat_n(text, s, sizeof s - 1)
	bool ok = said ? dstr_cat(text, said)
	        : cut ? dstr_cat_n(text, body.data, cut)
	        : body.len ? QUOTE("a response that is not text")
	        : QUOTE("empty response");
#undef QUOTE
	json_object_put(event);
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
	return AI_GEMINI >> api & 1 ? &gemini : nullptr;
}

int
ai_gemini_url (char const  *url,
               char const  *path,
               char const  *tail,
               struct dstr *out)
{
	static char const api[] = "/v1beta/";
	static char const key[] = "key=";
	if (!url || !path || !out)
		return EINVAL;
	char const *scheme = strstr(url, "://");
	if (!scheme)
		return ENOENT;
	char const *host = scheme + sizeof "://" - 1;
	size_t end = strcspn(host, "?#");
	char const *root = memmem(host, end, api, sizeof api - 1);
	if (!root)
		return ENOENT;
	/* The last parameter key of the query, as the API reads it. */
	struct cstr_view found = { "", 0 };
	for (char const *p = host + end; *p == '?' || *p == '&'; ) {
		size_t len = strcspn(p + 1, "&#");
		if (!strncmp(p + 1, key, sizeof key - 1))
			found = (struct cstr_view){ p + 1, len };
		p += 1 + len;
	}
	return dstr_printf(out, "%.*s%s%s%s%.*s", (int)(root - url), url, path,
	                   tail ? tail : "", found.len ? "?" : "", (int)found.len, found.data)
	       ? 0 : ENOMEM;
}

int
ai_upload_url (struct ai_endpoint const *ep,
               struct dstr              *out)
{
	struct ai_upload const *up = ep ? ai_upload(ep->api) : nullptr;
	return up ? ai_gemini_url(ep->url, up->path, nullptr, out) : ENOENT;
}

int
ai_model_url (struct ai_endpoint const *ep,
              struct dstr              *out)
{
	bool gemini = ep && ep->model && AI_GEMINI >> ep->api & 1;
	return gemini ? ai_gemini_url(ep->url, "/v1beta/models/", ep->model, out) : ENOENT;
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
ai_uploaded (enum ai_api       api,
             long              status,
             struct cstr_view  body,
             struct dstr      *uri,
             int64_t          *expires,
             struct dstr      *why)
{
	if (!uri || !expires || !why || (!body.data && body.len))
		return EINVAL;
	*expires = 0;
	if (!ai_upload(api))
		return dstr_cat(why, "The upstream takes no files.") ? EIO : ENOMEM;
	struct json_object *root = body.len ? jb_parse(body.data, body.len, JSON_TOKENER_STRICT, 8)
	                                    : nullptr;
	struct json_object *file = jb_get(root, "file");
	struct cstr_view at = jb_view(jb_get(file, "uri"));
	int e;
	if (status < 200 || status > 299 || !at.data) {
		e = ai_describe_error(why, status, root, body);
		e = e ? e : EIO;
	} else if (!at.len || at.len > strspn(at.data, ai_uri_bytes)) {
		e = dstr_cat(why, "The upstream named the file with a URI that is not "
		             "printable ASCII.") ? EIO : ENOMEM;
	} else {
		*expires = ai_time(jb_view(jb_get(file, "expirationTime")).data);
		e = dstr_cat_n(uri, at.data, at.len) ? 0 : ENOMEM;
	}
	json_object_put(root);
	return e;
}

int
ai_answer (enum ai_api         api,
           long                status,
           struct json_object *root,
           struct cstr_view    body,
           struct dstr        *text,
           bool               *failed)
{
	*failed = status < 200 || status > 299 || !root || jb_get(root, "error");
	if (*failed)
		return ai_describe_error(text, status, root, body);

	if (!api || api >= array_size(ai_bodies))
		return EINVAL;
	char const *reason = nullptr;
	int e = ai_bodies[api].answer(root, text, nullptr, &reason);
	if (e)
		return e;
	if (!dstr_len(text)) {
		*failed = true;
		char why[AI_NO_ANSWER];
		return dstr_cat_n(text, why, ai_no_answer(why, sizeof why, reason)) ? 0 : ENOMEM;
	}
	if (ai_cut(api, reason))
		return dstr_cat_n(text, AI_CUT_NOTE, sizeof AI_CUT_NOTE - 1) ? 0 : ENOMEM;
	return 0;
}

int
ai_thoughts (enum ai_api         api,
             struct json_object *root,
             struct dstr        *text)
{
	if (!api || api >= array_size(ai_bodies))
		return EINVAL;
	char const *reason;
	return ai_bodies[api].answer(root, nullptr, text, &reason);
}
