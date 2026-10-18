/** @file Paired-model conversation state and reasoning-trace exchange. */
#include <errno.h>
#include <fcntl.h>
#include <json-c/json.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

#include "app.h"
#include "compat.h"
#include "dstr.h"
#include "http.h"
#include "list.h"
#include "loop.h"
#include "sse.h"
#include "util.h"

struct app_turn {
	struct list hook;
	struct dstr user[2];
	struct dstr answer[2];
	struct dstr reasoning[2];
};

struct app_prompt {
	struct list hook;
	struct dstr text;
};

struct app;

enum app_probe_stage {
	APP_PROBE_HISTORY,
	APP_PROBE_RAPID_BASE,
	APP_PROBE_RAPID_FULL
};

/* A check of a llama model's chat template, through its /apply-template. */
struct app_probe {
	struct http_request  *request;
	struct dstr           response;
	struct app           *app;
	struct dstr           rapid_base;
	size_t                index;
	enum app_probe_stage  stage;
	uint32_t              done;        /* bool, widened over the padding */
};

/* A model's streamed request: its events go to the parser, and the last
 * bytes of the response are kept to show if it fails. */
struct app_stream {
	struct http_request *request;
	struct sse           sse;
	struct dstr          tail;
	struct app          *app;
	size_t               index;
	int                  error;
	uint16_t             done;   /* bool, widened over the padding */
	uint16_t             ended;  /* bool, likewise: the stream said so */
};

/* A model's part of a turn in the chat mode. */
struct app_chat {
	struct app_stream stream;
	struct dstr       answer;
	struct dstr       reasoning;
};

enum app_rapid_stage {
	APP_RAPID_IDLE,
	APP_RAPID_TEMPLATE,
	APP_RAPID_THINK,
	APP_RAPID_ANSWER
};

/* A model's part of a turn in rapid mode. */
struct app_rapid {
	struct app_stream stream;
	struct dstr       response;
	struct dstr       base;
	struct dstr       emitted;
	struct dstr       chunk;
	struct dstr       answer;
	uint32_t          tokens;
	uint32_t          requested;
	uint32_t          stop_type_seen;  /* bool, widened over the padding */
	uint32_t          hit_limit;       /* bool, likewise */
};

struct app {
	struct app_probe      probe[2];
	struct app_chat       chat[2];
	struct app_rapid      rapid[2];
	struct app_cfg        cfg;
	struct loop_ref       stdin_ref;
	struct list           history;
	struct list           prompts;
	struct app_turn      *pending;
	struct http          *http;
	struct loop           loop;
	struct dstr           stdin_buf;
	struct dstr           bearer;
	struct dstr           template_url[2];
	struct dstr           completion_url[2];
	struct dstr           generation_tail[2];
	struct dstr           reasoning_tail[2];
	struct dstr           transition[2];
	long                  crosstalk_left;
	uint32_t              rapid_used;
	uint32_t              rapid_round;
	enum app_rapid_stage  rapid_stage;
	int                   stdin_flags;
	int                   error;
	uint16_t              stdin_eof;        /* bool, widened over the padding */
	uint16_t              exit_after_turn;  /* bool, likewise */
};

/* How a request ended, as curl's done callback has it. */
struct app_reply {
	char const *curl_error;
	long        status;
	CURLcode    result;
};

static char const probe_reasoning_old[] = "__TWINCEPTION_REASONING_PROBE_OLD_196F89C7__";
static char const probe_reasoning_new[] = "__TWINCEPTION_REASONING_PROBE_NEW_5A2E4D31__";
static char const probe_rapid_reasoning[] = "__TWINCEPTION_RAPID_REASONING_8E6A09B3__";
static char const probe_rapid_answer[] = "__TWINCEPTION_RAPID_ANSWER_71C4F052__";

static int app_advance (struct app *app);
static int app_advance_rapid (struct app *app);
static int app_start_turn (struct app *app, struct dstr const *user_a,
                           struct dstr const *user_b);
static http_done_cb app_probe_done;

static void
app_fail (struct app *app,
          int         error)
{
	if (!app->error)
		app->error = error;
	loop_exit(&app->loop);
}

static void
app_turn_destroy (struct app_turn **p_turn)
{
	if (!p_turn || !*p_turn)
		return;
	struct app_turn *turn = *p_turn;
	*p_turn = nullptr;
	for (size_t i = 0; i < array_size(turn->user); ++i) {
		dstr_cleanup(&turn->reasoning[i]);
		dstr_cleanup(&turn->answer[i]);
		dstr_cleanup(&turn->user[i]);
	}
	free(turn);
}

static struct app_turn *
app_turn_create (struct dstr const *user_a,
                 struct dstr const *user_b,
                 int               *e)
{
	struct app_turn *turn = malloc(sizeof *turn);
	if (!turn) {
		*e = ENOMEM;
		return nullptr;
	}
	*turn = (struct app_turn){
		.user = { DSTR_INIT, DSTR_INIT },
		.answer = { DSTR_INIT, DSTR_INIT },
		.reasoning = { DSTR_INIT, DSTR_INIT }
	};
	list_init(&turn->hook);

	*e = dstr_cat_n(&turn->user[0], dstr_get(user_a), dstr_len(user_a)) &&
	     dstr_cat_n(&turn->user[1], dstr_get(user_b), dstr_len(user_b))
	     ? 0 : ENOMEM;
	if (*e)
		app_turn_destroy(&turn);
	return turn;
}

static void
app_prompt_destroy (struct app_prompt **p_prompt)
{
	if (!p_prompt || !*p_prompt)
		return;
	struct app_prompt *prompt = *p_prompt;
	*p_prompt = nullptr;
	dstr_cleanup(&prompt->text);
	free(prompt);
}

static struct app_prompt *
app_prompt_create (char const *text,
                   size_t      len,
                   int        *e)
{
	struct app_prompt *prompt = malloc(sizeof *prompt);
	if (!prompt) {
		*e = ENOMEM;
		return nullptr;
	}
	*prompt = (struct app_prompt){ .text = DSTR_INIT };
	list_init(&prompt->hook);

	*e = dstr_cat_n(&prompt->text, text, len) ? 0 : ENOMEM;
	if (*e)
		app_prompt_destroy(&prompt);
	return prompt;
}

/* Set @p obj's @p key to @p val, which it takes: ENOMEM if @p val is
 * nullptr, a value that could not be made, or cannot be added. */
static int
app_json_set (struct json_object *obj,
              char const         *key,
              struct json_object *val)
{
	if (val && !json_object_object_add(obj, key, val))
		return 0;
	json_object_put(val);
	return ENOMEM;
}

/* An array of @p val alone, which it takes; nullptr if out of memory. */
static struct json_object *
app_json_list (struct json_object *val)
{
	struct json_object *arr = json_object_new_array();
	if (arr && val && !json_object_array_add(arr, val))
		return arr;
	json_object_put(val);
	json_object_put(arr);
	return nullptr;
}

/* @p obj's member @p key, if it is a string; else nullptr. */
static struct json_object *
app_json_string (struct json_object const *obj,
                 char const               *key)
{
	struct json_object *val;
	return json_object_object_get_ex(obj, key, &val) &&
	       json_object_is_type(val, json_type_string) ? val : nullptr;
}

/* Append the string @p str, unless it is nullptr, to @p out. */
static int
app_json_cat (struct json_object *str,
              struct dstr        *out)
{
	return !str || dstr_cat_n(out, json_object_get_string(str),
	                          (size_t)json_object_get_string_len(str))
	       ? 0 : ENOMEM;
}

/* The first of @p root's choices; nullptr if it has none. */
static struct json_object *
app_json_choice (struct json_object const *root)
{
	struct json_object *choices;
	return json_object_object_get_ex(root, "choices", &choices) &&
	       json_object_is_type(choices, json_type_array)
	       ? json_object_array_get_idx(choices, 0) : nullptr;
}

/* Append a message of @p role saying @p content, with @p reasoning as its
 * reasoning_content unless that is nullptr, to @p messages. */
static int
app_json_message (struct json_object *messages,
                  char const         *role,
                  char const         *content,
                  char const         *reasoning)
{
	struct json_object *msg = json_object_new_object();
	if (!msg)
		return ENOMEM;
	if (app_json_set(msg, "role", json_object_new_string(role)) ||
	    app_json_set(msg, "content", json_object_new_string(content)) ||
	    (reasoning && app_json_set(msg, "reasoning_content",
	                               json_object_new_string(reasoning))) ||
	    json_object_array_add(messages, msg)) {
		json_object_put(msg);
		return ENOMEM;
	}
	return 0;
}

/* DeepInfra's switches for a model to reason, and to keep the reasoning it
 * is given. */
static int
app_json_deepinfra (struct json_object *root)
{
	struct json_object *reasoning = json_object_new_object();
	if (app_json_set(root, "reasoning", reasoning) ||
	    app_json_set(reasoning, "enabled", json_object_new_boolean(1)))
		return ENOMEM;

	struct json_object *kwargs = json_object_new_object();
	return app_json_set(root, "chat_template_kwargs", kwargs) ||
	       app_json_set(kwargs, "enable_thinking", json_object_new_boolean(1)) ||
	       app_json_set(kwargs, "preserve_thinking", json_object_new_boolean(1))
	       ? ENOMEM : 0;
}

static struct dstr const *
app_answer (struct app const      *app,
            struct app_turn const *turn,
            size_t                 model)
{
	switch (app->cfg.history_mode) {
	case APP_HISTORY_SHARED_A:
		return &turn->answer[0];
	case APP_HISTORY_SHARED_B:
		return &turn->answer[1];
	default:
		return &turn->answer[model];
	}
}

/* Append the history, as model @p model sees it, to @p messages. */
static int
app_history_json (struct app const   *app,
                  struct json_object *messages,
                  size_t              model)
{
	struct list *node;
	list_foreach (node, &app->history) {
		struct app_turn const *turn = container_of(node, struct app_turn, hook);
		if (app_json_message(messages, "user", dstr_get(&turn->user[model]),
		                     nullptr) ||
		    app_json_message(messages, "assistant",
		                     dstr_get(app_answer(app, turn, model)),
		                     dstr_get(&turn->reasoning[1 - model])))
			return ENOMEM;
	}
	return 0;
}

/* A request body whose "messages", in @p *messages, begin with the system
 * message, if there is one; nullptr if out of memory. */
static struct json_object *
app_body (struct app const    *app,
          struct json_object **messages)
{
	struct json_object *root = json_object_new_object();
	if (!root)
		return nullptr;
	*messages = json_object_new_array();
	if (app_json_set(root, "messages", *messages) ||
	    (app->cfg.system &&
	     app_json_message(*messages, "system", app->cfg.system, nullptr))) {
		json_object_put(root);
		return nullptr;
	}
	return root;
}

/* As app_body(), the history as model @p model sees it and the pending
 * turn's user message to it following. */
static struct json_object *
app_pending_body (struct app const    *app,
                  size_t               model,
                  struct json_object **messages)
{
	struct json_object *root = app_body(app, messages);
	if (root && (app_history_json(app, *messages, model) ||
	             app_json_message(*messages, "user",
	                              dstr_get(&app->pending->user[model]),
	                              nullptr))) {
		json_object_put(root);
		return nullptr;
	}
	return root;
}

/* POST @p body and release it; a null @p body, which failed to build, is
 * ENOMEM. */
static int
app_post (struct app          *app,
          size_t               model,
          char const          *url,
          struct json_object  *body,
          http_write_cb       *write_cb,
          http_done_cb        *done_cb,
          void                *user,
          struct http_request **request)
{
	if (!body)
		return ENOMEM;

	size_t len;
	char const *json = json_object_to_json_string_length(body,
	                                                     JSON_C_TO_STRING_PLAIN,
	                                                     &len);
	char const *headers[] = { dstr_get(&app->bearer), nullptr };
	int e = json ? http_post_json(app->http, url, json, len, true,
	                              app->cfg.provider[model] == APP_PROVIDER_DEEPINFRA
	                              ? headers : nullptr,
	                              0, write_cb, done_cb, user, request)
	             : ENOMEM;
	json_object_put(body);
	return e;
}

/* Show @p reply to a failed request of model @p model, after @p what. */
static void
app_reply_report (struct app_reply const *reply,
                  size_t                  model,
                  char const             *what)
{
	char const *curl_error = reply->curl_error;
	char const *sep = ": ";
	if (!curl_error || !*curl_error)
		sep = curl_error = "";
	fprintf(stderr, "model %c %s: HTTP %ld, curl=%s%s%s\n",
	        (int)('A' + model), what, reply->status,
	        curl_easy_strerror(reply->result), sep, curl_error);
}

/* EIO, shown as app_reply_report() does, unless @p reply is a success. */
static int
app_reply_check (struct app_reply const *reply,
                 size_t                  model,
                 char const             *what)
{
	if (reply->result == CURLE_OK && reply->status >= 200 && reply->status < 300)
		return 0;
	app_reply_report(reply, model, what);
	return EIO;
}

static void
app_probe_fini (struct app_probe *probe)
{
	http_cancel(probe->app->http, &probe->request);
	dstr_cleanup(&probe->rapid_base);
	dstr_cleanup(&probe->response);
}

static struct json_object *
app_probe_body (struct app_probe const *probe)
{
	static struct {
		char const *role;
		char const *content;
		char const *reasoning;
	} const msg[] = {
		{ "user",      "template probe 0",        nullptr               },
		{ "assistant", "template probe answer 0", probe_reasoning_old   },
		{ "user",      "template probe 1",        nullptr               },
		{ "assistant", "template probe answer 1", probe_reasoning_new   },
		{ "user",      "template probe 2",        nullptr               },
		{ "user",      "rapid template probe",    nullptr               },
		{ "assistant", probe_rapid_answer,        probe_rapid_reasoning }
	};
	/* Which of these each stage posts. */
	static struct {
		uint8_t first;
		uint8_t end;
	} const span[] = {
		[APP_PROBE_HISTORY]    = { 0, 5 },
		[APP_PROBE_RAPID_BASE] = { 5, 6 },
		[APP_PROBE_RAPID_FULL] = { 5, 7 }
	};

	struct json_object *messages;
	struct json_object *root = app_body(probe->app, &messages);
	if (!root)
		return nullptr;
	for (size_t i = span[probe->stage].first; i < span[probe->stage].end; ++i) {
		if (app_json_message(messages, msg[i].role, msg[i].content,
		                     msg[i].reasoning)) {
			json_object_put(root);
			return nullptr;
		}
	}
	return root;
}

static size_t
app_probe_write (char const *data,
                 size_t      len,
                 void       *user)
{
	struct app_probe *probe = user;
	return dstr_cat_n(&probe->response, data, len) ? len : 0;
}

/* The template check of @p probe's model failed, as @p why says: EPROTO. */
static int
app_probe_reject (struct app_probe const *probe,
                  char const             *why)
{
	fprintf(stderr, "model %c template check failed: %s\n",
	        (int)('A' + probe->index), why);
	return EPROTO;
}

/* Append the prompt from the probe's response to @p prompt. */
static int
app_probe_prompt (struct app_probe const *probe,
                  struct dstr            *prompt)
{
	if (!dstr_len(&probe->response))
		return app_probe_reject(probe, "empty response");

	struct json_object *root = json_tokener_parse(dstr_get(&probe->response));
	if (!root)
		return app_probe_reject(probe, "invalid JSON response");

	struct json_object *value = app_json_string(root, "prompt");
	int e = value ? app_json_cat(value, prompt)
	              : app_probe_reject(probe, "response has no prompt string");
	json_object_put(root);
	return e;
}

static int
app_probe_history (struct app_probe const *probe,
                   char const             *prompt)
{
	if (!strstr(prompt, probe_reasoning_new))
		return app_probe_reject(probe, "reasoning_content is absent "
		                               "from /apply-template output");

	if (!strstr(prompt, probe_reasoning_old)) {
		fprintf(stderr,
		        "model %c template check: older reasoning is pruned; "
		        "use --reasoning-preserve for full-history swapping\n",
		        (int)('A' + probe->index));
	} else if (probe->app->cfg.debug) {
		fprintf(stderr,
		        "model %c template check: reasoning_content survives full history\n",
		        (int)('A' + probe->index));
	}
	return 0;
}

static int
app_probe_rapid (struct app_probe const *probe,
                 char const             *prompt)
{
	size_t skip = sizeof probe_rapid_reasoning - 1;
	char const *reasoning = strstr(prompt, probe_rapid_reasoning);
	char const *answer = reasoning ? strstr(reasoning + skip, probe_rapid_answer)
	                               : nullptr;
	if (!answer) {
		fprintf(stderr,
		        "model %c rapid template check failed: cannot locate generation, "
		        "reasoning and answer prefills in /apply-template output\n",
		        (int)('A' + probe->index));
		return EPROTO;
	}

	char const *base = dstr_get(&probe->rapid_base);
	size_t base_len = dstr_len(&probe->rapid_base);
	size_t common = 0;
	while (base[common] && base[common] == prompt[common])
		++common;
	size_t reasoning_off = (size_t)(reasoning - prompt);
	if (common > reasoning_off) {
		fprintf(stderr,
		        "model %c rapid template check failed: generation prompt overlaps "
		        "reasoning payload\n",
		        (int)('A' + probe->index));
		return EPROTO;
	}

	char const *begin = reasoning + skip;
	size_t transition_len = (size_t)(answer - begin);
	if (!transition_len)
		return EPROTO;

	/* Each probe gets here once, so these start out empty. */
	struct app *app = probe->app;
	size_t i = probe->index;
	if (!dstr_cat_n(&app->generation_tail[i], base + common, base_len - common) ||
	    !dstr_cat_n(&app->reasoning_tail[i], prompt + common,
	                reasoning_off - common) ||
	    !dstr_cat_n(&app->transition[i], begin, transition_len))
		return ENOMEM;

	if (app->cfg.debug)
		fprintf(stderr,
		        "model %c rapid template check: generation tail %zu bytes, "
		        "reasoning prefix %zu bytes, reasoning-to-answer transition %zu bytes\n",
		        (int)('A' + probe->index), base_len - common,
		        reasoning_off - common, transition_len);
	return 0;
}

static int
app_probe_check (struct app_probe const *probe)
{
	dstr_auto prompt = DSTR_INIT;
	int e = app_probe_prompt(probe, &prompt);
	if (e)
		return e;

	return probe->stage == APP_PROBE_HISTORY
	       ? app_probe_history(probe, dstr_get(&prompt))
	       : app_probe_rapid(probe, dstr_get(&prompt));
}

/* Post @p probe's stage to its model's /apply-template. */
static int
app_probe_post (struct app_probe *probe)
{
	struct app *app = probe->app;
	struct dstr const *url = &app->template_url[probe->index];
	if (!dstr_len(url)) {
		fprintf(stderr,
		        "cannot derive /apply-template from model %c URL%s\n",
		        (int)('A' + probe->index),
		        app->cfg.rapid_quantum
		        ? "; rapid mode requires llama-server-compatible endpoints"
		        : "; use -P to skip template validation");
		return EINVAL;
	}

	dstr_clear(&probe->response);
	return app_post(app, probe->index, dstr_get(url), app_probe_body(probe),
	                app_probe_write, app_probe_done, probe, &probe->request);
}

/* Take in @p reply to @p probe's stage: the next stage is posted, or the
 * probe is done and the app goes on. */
static int
app_probe_end (struct app_probe       *probe,
               struct app_reply const *reply)
{
	int e = app_reply_check(reply, probe->index, "template check failed");
	if (e)
		return e;

	if (probe->stage != APP_PROBE_RAPID_BASE) {
		e = app_probe_check(probe);
		if (e)
			return e;
		probe->done = 1;
		return app_advance(probe->app);
	}

	/* The base stage runs once, so rapid_base starts out empty. */
	e = app_probe_prompt(probe, &probe->rapid_base);
	if (e)
		return e;
	probe->stage = APP_PROBE_RAPID_FULL;
	return app_probe_post(probe);
}

static void
app_probe_done (CURLcode    result,
                long        status,
                char const *curl_error,
                void       *user)
{
	struct app_probe *probe = user;
	struct app_reply const reply = { curl_error, status, result };
	probe->request = nullptr;
	int e = app_probe_end(probe, &reply);
	if (e)
		app_fail(probe->app, e);
}

/* Begin the template check of @p probe's model: a llama model must pass
 * one, unless -P skips it, which rapid mode does not allow.  The probe of
 * any other model is done at once. */
static int
app_probe_begin (struct app_probe *probe)
{
	struct app_cfg const *cfg = &probe->app->cfg;
	if (cfg->provider[probe->index] == APP_PROVIDER_LLAMA) {
		if (cfg->template_check)
			return app_probe_post(probe);
		if (cfg->rapid_quantum) {
			fputs("rapid mode requires template transition discovery; "
			      "-P cannot be used with --rapid for llama providers\n", stderr);
			return EINVAL;
		}
	}
	probe->done = 1;
	return 0;
}

/* Make @p s ready for a request. */
static void
app_stream_reset (struct app_stream *s)
{
	sse_reset(&s->sse);
	dstr_clear(&s->tail);
	s->error = 0;
	s->done = 0;
	s->ended = 0;
}

static void
app_stream_fini (struct app_stream *s)
{
	http_cancel(s->app->http, &s->request);
	sse_fini(&s->sse);
	dstr_cleanup(&s->tail);
}

static void
app_stream_tail (struct app_stream *s,
                 char const        *data,
                 size_t             len)
{
	enum { MAX_TAIL = 8192 };
	if (len >= MAX_TAIL) {
		dstr_clear(&s->tail);
		(void)dstr_cat_n(&s->tail, data + len - MAX_TAIL, MAX_TAIL);
		return;
	}

	size_t tail_len = dstr_len(&s->tail);
	if (tail_len + len > MAX_TAIL)
		dstr_erase(&s->tail, 0, tail_len + len - MAX_TAIL);
	(void)dstr_cat_n(&s->tail, data, len);
}

static size_t
app_stream_write (char const *data,
                  size_t      len,
                  void       *user)
{
	struct app_stream *s = user;
	app_stream_tail(s, data, len);
	int e = sse_feed(&s->sse, data, len);
	if (e) {
		s->error = e;
		return 0;
	}
	return len;
}

/* The JSON of an event of @p s, or nullptr with the error in @p *e: 0 for
 * the [DONE] that ends the stream, EPROTO for an event that is not JSON,
 * EREMOTEIO for one that reports an error, or ENOMEM. */
static struct json_object *
app_stream_event (struct app_stream *s,
                  char const        *data,
                  size_t             len,
                  int               *e)
{
	static char const done[] = "[DONE]";
	*e = 0;
	if (len == sizeof done - 1 && !memcmp(data, done, len)) {
		s->ended = 1;
		return nullptr;
	}

	struct json_tokener *tok = json_tokener_new();
	if (!tok) {
		*e = ENOMEM;
		return nullptr;
	}
	struct json_object *root = json_tokener_parse_ex(tok, data, (int)len);
	enum json_tokener_error je = json_tokener_get_error(tok);
	json_tokener_free(tok);
	if (je != json_tokener_success || !root) {
		json_object_put(root);
		*e = EPROTO;
		return nullptr;
	}

	if (json_object_object_get_ex(root, "error", nullptr)) {
		json_object_put(root);
		*e = EREMOTEIO;
		return nullptr;
	}
	return root;
}

/* The error, if any, that the request of @p s ended with: its own, the
 * parser's, or what @p reply or a missing end of the stream says. */
static int
app_stream_error (struct app_stream      *s,
                  struct app_reply const *reply)
{
	if (s->error)
		return s->error;
	int e = sse_end(&s->sse);
	if (e)
		return e;
	if (reply->result != CURLE_OK)
		return EIO;
	return reply->status < 200 || reply->status >= 300 || !s->ended ? EPROTO : 0;
}

/* As app_stream_error(), an error shown as app_reply_report() does, with
 * the tail of the response. */
static int
app_stream_check (struct app_stream      *s,
                  struct app_reply const *reply,
                  char const             *what)
{
	int e = app_stream_error(s, reply);
	if (!e)
		return 0;

	app_reply_report(reply, s->index, what);
	int tail_len = (int)dstr_len(&s->tail);
	if (tail_len)
		fprintf(stderr, "model %c response tail:\n%.*s\n",
		        (int)('A' + s->index), tail_len, dstr_get(&s->tail));
	return e;
}

/* The request of @p s is over, with the error @p e, if any. */
static void
app_stream_end (struct app_stream *s,
                int                e)
{
	s->request = nullptr;
	s->done = 1;
	s->error = e;
}

static void
app_print (struct app const      *app,
           struct app_turn const *turn)
{
	if (app->cfg.debug)
		printf("\n--- A reasoning -> B ---\n\033[1;35m%s\033[0m\n"
		       "\n--- B reasoning -> A ---\n\033[1;36m%s\033[0m\n",
		       dstr_get(&turn->reasoning[0]), dstr_get(&turn->reasoning[1]));

	printf("\n[A]\n\033[1;35m%s\033[0m\n\n[B]\n\033[1;36m%s\033[0m\n",
	       dstr_get(&turn->answer[0]), dstr_get(&turn->answer[1]));
	fflush(stdout);
}

/* Drop the pending turn, a request of which failed, and go on. */
static int
app_discard (struct app *app)
{
	fprintf(stderr, "%spaired turn discarded; history unchanged\n",
	        app->cfg.rapid_quantum ? "rapid " : "");
	app_turn_destroy(&app->pending);
	app->rapid_stage = APP_RAPID_IDLE;
	app->crosstalk_left = 0;
	return app_advance(app);
}

/* Take the pending turn, answered, into the history and show it; then go on
 * with a crosstalk round, if one is due, or else the next prompt. */
static int
app_commit (struct app *app)
{
	struct app_turn *turn = app->pending;
	app->pending = nullptr;
	list_append(&app->history, &turn->hook);
	app_print(app, turn);
	if (app->exit_after_turn || !app->crosstalk_left)
		return app_advance(app);

	if (app->crosstalk_left > 0)
		--app->crosstalk_left;
	if (app->cfg.debug)
		fprintf(stderr, "crosstalk: A <- B, B <- A%s\n",
		        app->crosstalk_left < 0 ? " (unbounded)" : "");
	int e = app_start_turn(app, &turn->answer[1], &turn->answer[0]);
	if (e)
		fprintf(stderr, "cannot start crosstalk round: %s\n", strerror(e));
	return e;
}

static int
app_finish_chat (struct app *app)
{
	struct app_chat *chat = app->chat;
	if (!app->pending || !chat[0].stream.done || !chat[1].stream.done)
		return 0;
	if (chat[0].stream.error || chat[1].stream.error)
		return app_discard(app);

	for (size_t i = 0; i < array_size(app->chat); ++i) {
		app->pending->answer[i] = dstr_move(&chat[i].answer);
		app->pending->reasoning[i] = dstr_move(&chat[i].reasoning);
	}
	return app_commit(app);
}

static void
app_chat_fini (struct app_chat *chat)
{
	app_stream_fini(&chat->stream);
	dstr_cleanup(&chat->reasoning);
	dstr_cleanup(&chat->answer);
}

static int
app_chat_delta (struct app_chat          *chat,
                struct json_object const *delta)
{
	struct json_object *reasoning = app_json_string(delta, "reasoning_content");
	if (!reasoning)
		reasoning = app_json_string(delta, "reasoning");
	int e = app_json_cat(reasoning, &chat->reasoning);
	if (e)
		return e;
	return app_json_cat(app_json_string(delta, "content"), &chat->answer);
}

static int
app_chat_event (char const *data,
                size_t      len,
                void       *user)
{
	struct app_chat *chat = user;
	int e;
	struct json_object *root = app_stream_event(&chat->stream, data, len, &e);
	if (!root)
		return e;

	struct json_object *delta;
	if (json_object_object_get_ex(app_json_choice(root), "delta", &delta))
		e = app_chat_delta(chat, delta);
	json_object_put(root);
	return e;
}

static void
app_chat_done (CURLcode    result,
               long        status,
               char const *curl_error,
               void       *user)
{
	struct app_stream *s = user;
	struct app_reply const reply = { curl_error, status, result };
	app_stream_end(s, app_stream_check(s, &reply, "failed"));
	int e = app_finish_chat(s->app);
	if (e)
		app_fail(s->app, e);
}

static struct json_object *
app_chat_body (struct app_chat const *chat)
{
	struct app const *app = chat->stream.app;
	struct app_cfg const *cfg = &app->cfg;
	size_t i = chat->stream.index;
	struct json_object *messages;
	struct json_object *root = app_pending_body(app, i, &messages);
	if (!root)
		return nullptr;

	if (app_json_set(root, "stream", json_object_new_boolean(1)) ||
	    (cfg->provider[i] == APP_PROVIDER_DEEPINFRA
	     ? app_json_deepinfra(root)
	     : app_json_set(root, "reasoning_format",
	                    json_object_new_string("deepseek"))) ||
	    (cfg->model[i] &&
	     app_json_set(root, "model", json_object_new_string(cfg->model[i]))) ||
	    (cfg->temperature >= 0 &&
	     app_json_set(root, "temperature",
	                  json_object_new_double(cfg->temperature))) ||
	    (cfg->max_tokens > 0 &&
	     app_json_set(root, "max_tokens",
	                  json_object_new_int64(cfg->max_tokens)))) {
		json_object_put(root);
		return nullptr;
	}
	return root;
}

static int
app_chat_post (struct app_chat *chat)
{
	struct app_stream *s = &chat->stream;
	app_stream_reset(s);
	dstr_clear(&chat->answer);
	dstr_clear(&chat->reasoning);
	return app_post(s->app, s->index, s->app->cfg.url[s->index],
	                app_chat_body(chat), app_stream_write, app_chat_done, s,
	                &s->request);
}

static int
app_start_chat (struct app *app)
{
	for (size_t i = 0; i < array_size(app->chat); ++i) {
		int e = app_chat_post(&app->chat[i]);
		if (e)
			return e;
	}
	return 0;
}

static void
app_rapid_fini (struct app_rapid *rapid)
{
	app_stream_fini(&rapid->stream);
	dstr_cleanup(&rapid->answer);
	dstr_cleanup(&rapid->chunk);
	dstr_cleanup(&rapid->emitted);
	dstr_cleanup(&rapid->base);
	dstr_cleanup(&rapid->response);
}

/* Make @p rapid ready for a request of @p requested tokens, 0 if it has no
 * limit. */
static void
app_rapid_reset (struct app_rapid *rapid,
                 uint32_t          requested)
{
	app_stream_reset(&rapid->stream);
	dstr_clear(&rapid->chunk);
	rapid->tokens = 0;
	rapid->requested = requested;
	rapid->stop_type_seen = 0;
	rapid->hit_limit = 0;
}

static size_t
app_rapid_template_write (char const *data,
                          size_t      len,
                          void       *user)
{
	struct app_rapid *rapid = user;
	return dstr_cat_n(&rapid->response, data, len) ? len : 0;
}

/* Append the prompt of @p rapid's /apply-template response to its base:
 * EPROTO if the response has none, or ENOMEM, either said to be invalid
 * JSON. */
static int
app_rapid_template_parse (struct app_rapid *rapid)
{
	struct json_object *root = json_tokener_parse(dstr_get(&rapid->response));
	struct json_object *prompt = app_json_string(root, "prompt");
	int e = prompt ? app_json_cat(prompt, &rapid->base) : EPROTO;
	json_object_put(root);
	if (e)
		fprintf(stderr, "model %c turn template returned invalid JSON\n",
		        (int)('A' + rapid->stream.index));
	return e;
}

static int
app_rapid_template_end (struct app_rapid       *rapid,
                        struct app_reply const *reply)
{
	int e = app_reply_check(reply, rapid->stream.index, "turn template failed");
	return e ? e : app_rapid_template_parse(rapid);
}

static void
app_rapid_template_done (CURLcode    result,
                         long        status,
                         char const *curl_error,
                         void       *user)
{
	struct app_rapid *rapid = user;
	struct app_reply const reply = { curl_error, status, result };
	app_stream_end(&rapid->stream, app_rapid_template_end(rapid, &reply));
	int e = app_advance_rapid(rapid->stream.app);
	if (e)
		app_fail(rapid->stream.app, e);
}

static struct json_object *
app_rapid_template_body (struct app_rapid const *rapid)
{
	struct app const *app = rapid->stream.app;
	size_t i = rapid->stream.index;
	char const *model = app->cfg.model[i];
	struct json_object *messages;
	struct json_object *root = app_pending_body(app, i, &messages);
	if (root && model &&
	    app_json_set(root, "model", json_object_new_string(model))) {
		json_object_put(root);
		return nullptr;
	}
	return root;
}

/* Begin @p rapid's part of the pending turn: a llama model's template
 * renders the turn's prompt, which DeepInfra renders itself. */
static int
app_rapid_begin (struct app_rapid *rapid)
{
	struct app_stream *s = &rapid->stream;
	struct app *app = s->app;
	app_rapid_reset(rapid, 0);
	dstr_clear(&rapid->response);
	dstr_clear(&rapid->base);
	dstr_clear(&rapid->emitted);
	dstr_clear(&rapid->answer);
	if (app->cfg.provider[s->index] == APP_PROVIDER_DEEPINFRA) {
		s->done = 1;
		return 0;
	}

	return app_post(app, s->index, dstr_get(&app->template_url[s->index]),
	                app_rapid_template_body(rapid), app_rapid_template_write,
	                app_rapid_template_done, rapid, &s->request);
}

/* Where @p rapid's text goes: the answer in its stage, the quantum's
 * reasoning otherwise. */
static struct dstr *
app_rapid_out (struct app_rapid *rapid)
{
	return rapid->stream.app->rapid_stage == APP_RAPID_ANSWER
	       ? &rapid->answer : &rapid->chunk;
}

/* The text of a DeepInfra @p choice's delta: the first of its reasoning
 * and its content that it has, but its content alone in the answer stage;
 * nullptr if that is no string. */
static struct json_object *
app_rapid_text (struct app_rapid const   *rapid,
                struct json_object const *choice)
{
	static char const *const keys[] = {
		"reasoning_content",
		"reasoning",
		"content"
	};
	struct json_object *delta;
	if (!json_object_object_get_ex(choice, "delta", &delta))
		return nullptr;

	size_t i = rapid->stream.app->rapid_stage == APP_RAPID_ANSWER
	           ? array_size(keys) - 1 : 0;
	struct json_object *text = nullptr;
	while (i < array_size(keys) &&
	       !json_object_object_get_ex(delta, keys[i], &text))
		++i;
	return json_object_is_type(text, json_type_string) ? text : nullptr;
}

/* Take in a DeepInfra event's first @p choice: its text, and whether it
 * finished at the token limit. */
static int
app_rapid_deepinfra_event (struct app_rapid         *rapid,
                           struct json_object const *choice)
{
	int e = app_json_cat(app_rapid_text(rapid, choice), app_rapid_out(rapid));
	struct json_object *reason = app_json_string(choice, "finish_reason");
	if (e || !reason)
		return e;

	rapid->stop_type_seen = 1;
	rapid->hit_limit = !strcmp(json_object_get_string(reason), "length");
	if (rapid->hit_limit)
		rapid->tokens = rapid->requested;
	return 0;
}

/* Count the tokens of a llama.cpp event of a quantum's reasoning. */
static int
app_rapid_count (struct app_rapid         *rapid,
                 struct json_object const *root)
{
	struct json_object *tokens;
	if (rapid->stream.app->rapid_stage != APP_RAPID_THINK ||
	    !json_object_object_get_ex(root, "tokens", &tokens) ||
	    !json_object_is_type(tokens, json_type_array))
		return 0;

	size_t n = json_object_array_length(tokens);
	if (n > UINT32_MAX - rapid->tokens)
		return EOVERFLOW;
	rapid->tokens += (uint32_t)n;
	return 0;
}

/* Take in a llama.cpp /completion event: its text and tokens, and whether
 * and why it stopped. */
static int
app_rapid_llama_event (struct app_rapid         *rapid,
                       struct json_object const *root)
{
	int e = app_json_cat(app_json_string(root, "content"), app_rapid_out(rapid));
	if (e)
		return e;
	e = app_rapid_count(rapid, root);
	if (e)
		return e;

	struct json_object *value = app_json_string(root, "stop_type");
	if (value) {
		rapid->stop_type_seen = 1;
		rapid->hit_limit = !strcmp(json_object_get_string(value), "limit");
	}
	if (json_object_object_get_ex(root, "stop", &value) &&
	    json_object_get_boolean(value))
		rapid->stream.ended = 1;
	return 0;
}

static int
app_rapid_event (char const *data,
                 size_t      len,
                 void       *user)
{
	struct app_rapid *rapid = user;
	struct app_stream *s = &rapid->stream;
	int e;
	struct json_object *root = app_stream_event(s, data, len, &e);
	if (!root)
		return e;

	e = s->app->cfg.provider[s->index] == APP_PROVIDER_DEEPINFRA
	    ? app_rapid_deepinfra_event(rapid, app_json_choice(root))
	    : app_rapid_llama_event(rapid, root);
	json_object_put(root);
	return e;
}

static void
app_rapid_done (CURLcode    result,
                long        status,
                char const *curl_error,
                void       *user)
{
	struct app_stream *s = user;
	struct app_reply const reply = { curl_error, status, result };
	app_stream_end(s, app_stream_check(s, &reply, "rapid request failed"));
	int e = app_advance_rapid(s->app);
	if (e)
		app_fail(s->app, e);
}

/* Append to @p messages the assistant message that @p rapid's model goes
 * on from: the other model's reasoning, as its own, closed for the answer. */
static int
app_rapid_prefill (struct app_rapid const *rapid,
                   struct json_object     *messages)
{
	static char const end[] = "</think>\n";
	struct app const *app = rapid->stream.app;
	struct dstr const *foreign = &app->rapid[1 - rapid->stream.index].emitted;
	dstr_auto prefill = dstr("<think>\n");
	if (!dstr_cat_n(&prefill, dstr_get(foreign), dstr_len(foreign)) ||
	    (app->rapid_stage == APP_RAPID_ANSWER &&
	     !dstr_cat_n(&prefill, end, sizeof end - 1)))
		return ENOMEM;
	return app_json_message(messages, "assistant", dstr_get(&prefill), nullptr);
}

static struct json_object *
app_rapid_deepinfra_body (struct app_rapid const *rapid,
                          long                    n_predict)
{
	struct app const *app = rapid->stream.app;
	struct app_cfg const *cfg = &app->cfg;
	size_t i = rapid->stream.index;
	struct json_object *messages;
	struct json_object *root = app_pending_body(app, i, &messages);
	if (!root)
		return nullptr;

	if (app_json_set(root, "stream", json_object_new_boolean(1)) ||
	    app_json_set(root, "model", json_object_new_string(cfg->model[i])) ||
	    app_json_set(root, "continue_final_message",
	                 json_object_new_boolean(1)) ||
	    app_json_deepinfra(root) ||
	    (cfg->temperature >= 0 &&
	     app_json_set(root, "temperature",
	                  json_object_new_double(cfg->temperature))) ||
	    (n_predict > 0 &&
	     app_json_set(root, "max_tokens", json_object_new_int64(n_predict))) ||
	    (app->rapid_stage == APP_RAPID_THINK &&
	     app_json_set(root, "stop", json_object_new_string("</think>"))) ||
	    app_rapid_prefill(rapid, messages)) {
		json_object_put(root);
		return nullptr;
	}
	return root;
}

/* The raw prompt that @p rapid's model goes on from, in @p prompt: the
 * turn's, up to its generation tail, then the reasoning tail and the other
 * model's reasoning, as its own, and for the answer the transition to it. */
static int
app_rapid_prompt (struct app_rapid const *rapid,
                  struct dstr            *prompt)
{
	struct app const *app = rapid->stream.app;
	size_t i = rapid->stream.index;
	/* The probe stores a nonempty transition along with both tails. */
	struct dstr const *transition = &app->transition[i];
	if (!dstr_len(transition))
		return EPROTO;

	struct dstr const *generation_tail = &app->generation_tail[i];
	struct dstr const *reasoning_tail = &app->reasoning_tail[i];
	struct dstr const *foreign = &app->rapid[1 - i].emitted;
	char const *base = dstr_get(&rapid->base);
	size_t base_len = dstr_len(&rapid->base);
	size_t generation_len = dstr_len(generation_tail);
	if (generation_len > base_len ||
	    memcmp(base + base_len - generation_len, dstr_get(generation_tail),
	           generation_len)) {
		fprintf(stderr,
		        "model %c rapid turn template no longer matches probed generation tail\n",
		        (int)('A' + i));
		return EPROTO;
	}

	if (!dstr_cat_n(prompt, base, base_len - generation_len) ||
	    !dstr_cat_n(prompt, dstr_get(reasoning_tail), dstr_len(reasoning_tail)) ||
	    !dstr_cat_n(prompt, dstr_get(foreign), dstr_len(foreign)) ||
	    (app->rapid_stage == APP_RAPID_ANSWER &&
	     !dstr_cat_n(prompt, dstr_get(transition), dstr_len(transition))))
		return ENOMEM;
	return 0;
}

/* A /completion request body for @p rapid's model, which stops at the
 * transition to the answer while it reasons. */
static struct json_object *
app_rapid_llama_body (struct app_rapid const *rapid,
                      struct dstr const      *prompt,
                      long                    n_predict)
{
	struct app const *app = rapid->stream.app;
	struct app_cfg const *cfg = &app->cfg;
	size_t i = rapid->stream.index;
	char const *stop = dstr_get(&app->transition[i]);
	size_t prompt_len = dstr_len(prompt);
	if (prompt_len > INT_MAX)
		return nullptr;
	struct json_object *root = json_object_new_object();
	if (!root)
		return nullptr;

	if (app_json_set(root, "prompt",
	                 json_object_new_string_len(dstr_get(prompt),
	                                            (int)prompt_len)) ||
	    app_json_set(root, "stream", json_object_new_boolean(1)) ||
	    app_json_set(root, "cache_prompt", json_object_new_boolean(1)) ||
	    (app->rapid_stage == APP_RAPID_THINK &&
	     app_json_set(root, "stop",
	                  app_json_list(json_object_new_string(stop)))) ||
	    (n_predict > 0 &&
	     app_json_set(root, "n_predict", json_object_new_int64(n_predict))) ||
	    (cfg->model[i] &&
	     app_json_set(root, "model", json_object_new_string(cfg->model[i]))) ||
	    (cfg->temperature >= 0 &&
	     app_json_set(root, "temperature",
	                  json_object_new_double(cfg->temperature)))) {
		json_object_put(root);
		return nullptr;
	}
	return root;
}

/* Post @p rapid's model the request of the stage, of at most @p n_predict
 * tokens if that is positive. */
static int
app_rapid_start (struct app_rapid *rapid,
                 long              n_predict)
{
	struct app_stream *s = &rapid->stream;
	struct app *app = s->app;
	app_rapid_reset(rapid, n_predict > 0 ? (uint32_t)n_predict : 0);
	if (app->cfg.provider[s->index] == APP_PROVIDER_DEEPINFRA)
		return app_post(app, s->index, app->cfg.url[s->index],
		                app_rapid_deepinfra_body(rapid, n_predict),
		                app_stream_write, app_rapid_done, s, &s->request);

	dstr_auto prompt = DSTR_INIT;
	int e = app_rapid_prompt(rapid, &prompt);
	if (e)
		return e;
	return app_post(app, s->index, dstr_get(&app->completion_url[s->index]),
	                app_rapid_llama_body(rapid, &prompt, n_predict),
	                app_stream_write, app_rapid_done, s, &s->request);
}

static int
app_start_stage (struct app           *app,
                 enum app_rapid_stage  stage,
                 long                  n_predict)
{
	app->rapid_stage = stage;
	for (size_t i = 0; i < array_size(app->rapid); ++i) {
		int e = app_rapid_start(&app->rapid[i], n_predict);
		if (e)
			return e;
	}
	return 0;
}

static int
app_start_quantum (struct app *app)
{
	uint32_t remaining = app->cfg.rapid_budget - app->rapid_used;
	uint32_t n = remaining < app->cfg.rapid_quantum ? remaining
	                                                : app->cfg.rapid_quantum;
	if (!n)
		return EINVAL;

	++app->rapid_round;
	return app_start_stage(app, APP_RAPID_THINK, (long)n);
}

static void
app_print_quantum (struct app const *app)
{
	if (!app->cfg.debug)
		return;
	fprintf(stdout,
	        "\n--- rapid %u: A -> B (%u tokens) ---\n%s\n"
	        "\n--- rapid %u: B -> A (%u tokens) ---\n%s\n",
	        app->rapid_round, app->rapid[0].tokens, dstr_get(&app->rapid[0].chunk),
	        app->rapid_round, app->rapid[1].tokens, dstr_get(&app->rapid[1].chunk));
	fflush(stdout);
}

/* Both models have reasoned a quantum, which each adds to what the other
 * goes on from: they answer once the budget is spent, or either stopped
 * short, and reason on if not. */
static int
app_end_quantum (struct app *app)
{
	app_print_quantum(app);
	uint32_t requested = app->rapid[0].requested;
	bool early = false;
	for (size_t i = 0; i < array_size(app->rapid); ++i) {
		struct app_rapid *rapid = &app->rapid[i];
		if (!dstr_cat_n(&rapid->emitted, dstr_get(&rapid->chunk),
		                dstr_len(&rapid->chunk)))
			return ENOMEM;
		early |= rapid->stop_type_seen ? !rapid->hit_limit
		                               : rapid->tokens < requested;
	}

	app->rapid_used += requested;
	if (!early)
		return app->rapid_used < app->cfg.rapid_budget
		       ? app_start_quantum(app)
		       : app_start_stage(app, APP_RAPID_ANSWER, app->cfg.max_tokens);

	if (app->cfg.debug)
		fprintf(stderr,
		        "rapid reasoning ended early at round %u; forcing answer transition\n",
		        app->rapid_round);
	return app_start_stage(app, APP_RAPID_ANSWER, app->cfg.max_tokens);
}

static int
app_commit_rapid (struct app *app)
{
	for (size_t i = 0; i < array_size(app->rapid); ++i) {
		app->pending->answer[i] = dstr_move(&app->rapid[i].answer);
		app->pending->reasoning[i] = dstr_move(&app->rapid[i].emitted);
	}
	app->rapid_stage = APP_RAPID_IDLE;
	return app_commit(app);
}

/* Go on from the rapid stage that both models are done with. */
static int
app_next_stage (struct app *app)
{
	switch (app->rapid_stage) {
	case APP_RAPID_TEMPLATE:
		return app_start_quantum(app);
	case APP_RAPID_THINK:
		return app_end_quantum(app);
	case APP_RAPID_ANSWER:
		return app_commit_rapid(app);
	case APP_RAPID_IDLE:
		break;
	}
	return EPROTO;
}

/* Go on with the rapid turn once both models are done with its stage. */
static int
app_advance_rapid (struct app *app)
{
	struct app_rapid const *rapid = app->rapid;
	if (!app->pending || !rapid[0].stream.done || !rapid[1].stream.done)
		return 0;
	if (rapid[0].stream.error || rapid[1].stream.error)
		return app_discard(app);
	return app_next_stage(app);
}

static int
app_start_rapid (struct app *app)
{
	app->rapid_stage = APP_RAPID_TEMPLATE;
	app->rapid_used = 0;
	app->rapid_round = 0;
	for (size_t i = 0; i < array_size(app->rapid); ++i) {
		int e = app_rapid_begin(&app->rapid[i]);
		if (e)
			return e;
	}
	/* DeepInfra models need no template, so the turn may go on at once. */
	return app_advance_rapid(app);
}

/* Start a turn that tells model A @p user_a and model B @p user_b. */
static int
app_start_turn (struct app        *app,
                struct dstr const *user_a,
                struct dstr const *user_b)
{
	int e;
	app->pending = app_turn_create(user_a, user_b, &e);
	if (!app->pending)
		return e;
	return app->cfg.rapid_quantum ? app_start_rapid(app) : app_start_chat(app);
}

static struct app_prompt *
app_pop_prompt (struct app *app)
{
	if (list_is_empty(&app->prompts))
		return nullptr;
	struct app_prompt *prompt = container_of(app->prompts.next,
	                                         struct app_prompt, hook);
	list_del(&prompt->hook);
	return prompt;
}

/* Start a turn on the next prompt, unless a turn or a template check is
 * under way; with no prompt, ask for one, or end once there will be none. */
static int
app_advance (struct app *app)
{
	if (app->pending || !app->probe[0].done || !app->probe[1].done)
		return 0;

	struct app_prompt *prompt = app_pop_prompt(app);
	if (prompt) {
		app->crosstalk_left = app->cfg.crosstalk_rounds;
		int e = app_start_turn(app, &prompt->text, &prompt->text);
		app_prompt_destroy(&prompt);
		if (e)
			fprintf(stderr, "cannot start request pair: %s\n", strerror(e));
		return e;
	}

	if (app->stdin_eof || app->exit_after_turn) {
		loop_exit(&app->loop);
		return 0;
	}

	fputs("> ", stdout);
	fflush(stdout);
	return 0;
}

static int
app_queue_prompt (struct app *app,
                  char const *text,
                  size_t      len)
{
	static char const quit[] = ":quit";
	static char const stop[] = ":stop";
	while (len && (text[len - 1] == '\n' || text[len - 1] == '\r'))
		--len;
	if (!len)
		return 0;

	if (len == sizeof quit - 1 && !memcmp(text, quit, len)) {
		app->exit_after_turn = 1;
		app->crosstalk_left = 0;
		if (!app->pending)
			loop_exit(&app->loop);
		return 0;
	}
	if (len == sizeof stop - 1 && !memcmp(text, stop, len)) {
		app->crosstalk_left = 0;
		return 0;
	}

	int e;
	struct app_prompt *prompt = app_prompt_create(text, len, &e);
	if (prompt)
		list_append(&app->prompts, &prompt->hook);
	return e;
}

/* Read stdin into the app's buffer until it would block, or ends. */
static int
app_stdin_drain (struct app *app)
{
	char data[4096];
	for (;;) {
		ssize_t n = read(STDIN_FILENO, data, sizeof data);
		int e = n < 0 ? errno : 0;
		if (e == EINTR)
			continue;
		if (e)
			return e == EAGAIN || e == EWOULDBLOCK ? 0 : e;
		if (!n) {
			app->stdin_eof = 1;
			return 0;
		}
		if (!dstr_cat_n(&app->stdin_buf, data, (size_t)n))
			return ENOMEM;
	}
}

/* Read stdin, and queue its lines, and once it has ended, the rest; then
 * go on. */
static int
app_stdin_read (struct app *app)
{
	int e = app_stdin_drain(app);
	if (e)
		return e;

	char const *input = dstr_get(&app->stdin_buf);
	size_t input_len = dstr_len(&app->stdin_buf);
	size_t used = 0;
	for (char const *nl; (nl = memchr(input + used, '\n', input_len - used)); ) {
		size_t len = (size_t)(nl - input) - used;
		e = app_queue_prompt(app, input + used, len);
		if (e)
			return e;
		used += len + 1;
	}
	dstr_erase(&app->stdin_buf, 0, used);
	if (app->stdin_eof) {
		e = app_queue_prompt(app, dstr_get(&app->stdin_buf),
		                     dstr_len(&app->stdin_buf));
		dstr_clear(&app->stdin_buf);
		if (e)
			return e;
	}
	return app_advance(app);
}

static void
app_stdin_event (struct loop     *loop,
                 struct loop_ref *ref,
                 uint32_t         events)
{
	struct app *app = ref->owner;
	if (events & (EPOLLHUP | EPOLLRDHUP))
		app->stdin_eof = 1;

	int e = app_stdin_read(app);
	/* A hangup stays readable, so a watch past the end would spin. */
	if (app->stdin_eof)
		loop_unwatch(loop, ref);
	if (e)
		app_fail(app, e);
}

/* Model @p i's /apply-template and /completion URLs, which replace the
 * chat-completions path of its URL; none if it has no such path. */
static int
app_urls (struct app *app,
          size_t      i)
{
	static char const chat[] = "/v1/chat/completions";
	static char const apply[] = "/apply-template";
	static char const completion[] = "/completion";
	char const *url = app->cfg.url[i];
	size_t len = strlen(url);
	size_t chat_len = sizeof chat - 1;
	if (len < chat_len || memcmp(url + len - chat_len, chat, chat_len))
		return 0;

	len -= chat_len;
	return dstr_cat_n(&app->template_url[i], url, len) &&
	       dstr_cat_n(&app->template_url[i], apply, sizeof apply - 1) &&
	       dstr_cat_n(&app->completion_url[i], url, len) &&
	       dstr_cat_n(&app->completion_url[i], completion,
	                  sizeof completion - 1) ? 0 : ENOMEM;
}

/* Watch stdin, which is made nonblocking, its flags kept to restore. */
static int
app_stdin_watch (struct app *app)
{
	int flags = fcntl(STDIN_FILENO, F_GETFL);
	if (flags < 0 || fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK) < 0)
		return errno;
	app->stdin_flags = flags;
	return loop_watch(&app->loop, &app->stdin_ref, STDIN_FILENO,
	                  EPOLLIN | EPOLLRDHUP, app_stdin_event, app);
}

static struct app_probe
app_probe (struct app *app,
           size_t      index)
{
	return (struct app_probe){
		.response = DSTR_INIT,
		.app = app,
		.rapid_base = DSTR_INIT,
		.index = index,
		.stage = app->cfg.rapid_quantum ? APP_PROBE_RAPID_BASE
		                                : APP_PROBE_HISTORY
	};
}

static struct app_stream
app_stream (struct app   *app,
            size_t        index,
            sse_event_cb *event,
            void         *user)
{
	return (struct app_stream){
		.sse = sse(event, user),
		.tail = DSTR_INIT,
		.app = app,
		.index = index
	};
}

static struct app_chat
app_chat (struct app *app,
          size_t      index)
{
	return (struct app_chat){
		.stream = app_stream(app, index, app_chat_event, &app->chat[index]),
		.answer = DSTR_INIT,
		.reasoning = DSTR_INIT
	};
}

static struct app_rapid
app_rapid (struct app *app,
           size_t      index)
{
	return (struct app_rapid){
		.stream = app_stream(app, index, app_rapid_event, &app->rapid[index]),
		.response = DSTR_INIT,
		.base = DSTR_INIT,
		.emitted = DSTR_INIT,
		.chunk = DSTR_INIT,
		.answer = DSTR_INIT
	};
}

/* What of the app does not depend on where it is; app_init() does the rest
 * in place. */
static struct app
app (struct app_cfg const *cfg)
{
	struct app ret = {
		.cfg = *cfg,
		.stdin_ref.fd = -1,
		.loop = loop(),
		.stdin_buf = DSTR_INIT,
		.bearer = DSTR_INIT,
		.template_url = { DSTR_INIT, DSTR_INIT },
		.completion_url = { DSTR_INIT, DSTR_INIT },
		.generation_tail = { DSTR_INIT, DSTR_INIT },
		.reasoning_tail = { DSTR_INIT, DSTR_INIT },
		.transition = { DSTR_INIT, DSTR_INIT },
		.stdin_flags = -1
	};
	return ret;
}

/* Set up @p app, which app() made, in place; on failure, app_fini() frees
 * what it has. */
static int
app_init (struct app *app)
{
	for (size_t i = 0; i < array_size(app->probe); ++i) {
		app->probe[i] = app_probe(app, i);
		app->chat[i] = app_chat(app, i);
		app->rapid[i] = app_rapid(app, i);
	}
	list_init(&app->history);
	list_init(&app->prompts);
	struct app_cfg const *cfg = &app->cfg;

	int e = loop_init(&app->loop);
	if (e)
		return e;

	for (size_t i = 0; i < array_size(app->template_url); ++i) {
		e = app_urls(app, i);
		if (e)
			return e;
	}

	if (cfg->deepinfra_key &&
	    !dstr_printf(&app->bearer, "Authorization: Bearer %s",
	                 cfg->deepinfra_key))
		return ENOMEM;

	app->http = http_create(&app->loop, nullptr, &e);
	if (!app->http)
		return e;

	if (!cfg->prompt)
		return app_stdin_watch(app);
	app->stdin_eof = 1;
	return app_queue_prompt(app, cfg->prompt, strlen(cfg->prompt));
}

static void
app_fini (struct app *app)
{
	for (size_t i = 0; i < array_size(app->probe); ++i) {
		app_probe_fini(&app->probe[i]);
		app_chat_fini(&app->chat[i]);
		app_rapid_fini(&app->rapid[i]);
		dstr_cleanup(&app->template_url[i]);
		dstr_cleanup(&app->completion_url[i]);
		dstr_cleanup(&app->generation_tail[i]);
		dstr_cleanup(&app->reasoning_tail[i]);
		dstr_cleanup(&app->transition[i]);
	}

	app_turn_destroy(&app->pending);
	struct list *node;
	struct list *tmp;
	list_foreach_safe (node, tmp, &app->prompts) {
		struct app_prompt *prompt = container_of(node, struct app_prompt, hook);
		list_del(node);
		app_prompt_destroy(&prompt);
	}
	list_foreach_safe (node, tmp, &app->history) {
		struct app_turn *turn = container_of(node, struct app_turn, hook);
		list_del(node);
		app_turn_destroy(&turn);
	}

	dstr_cleanup(&app->stdin_buf);
	dstr_cleanup(&app->bearer);
	loop_unwatch(&app->loop, &app->stdin_ref);
	if (app->stdin_flags >= 0)
		(void)fcntl(STDIN_FILENO, F_SETFL, app->stdin_flags);
	http_destroy(&app->http);
	loop_fini(&app->loop);
}

/* Set up @p app, check the templates, then take turns until the prompts
 * run out or one fails: its error, or the loop's. */
static int
app_exec (struct app *app)
{
	int e = app_init(app);
	if (e)
		return e;
	for (size_t i = 0; i < array_size(app->probe); ++i) {
		e = app_probe_begin(&app->probe[i]);
		if (e)
			return e;
	}
	e = app_advance(app);
	if (e)
		return e;
	e = loop_exec(&app->loop);
	return e ? e : app->error;
}

int
app_run (struct app_cfg const *cfg)
{
	struct app a = app(cfg);
	int e = app_exec(&a);
	app_fini(&a);
	return e;
}
