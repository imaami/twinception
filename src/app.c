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
#include "dstr.h"
#include "http.h"
#include "list.h"
#include "loop.h"
#include "sse.h"
#include "util.h"

struct turn {
	struct list hook;
	struct dstr user[2];
	struct dstr answer[2];
	struct dstr reasoning[2];
};

struct prompt {
	struct list hook;
	struct dstr text;
};

struct app;

enum template_probe_stage {
	TEMPLATE_PROBE_HISTORY,
	TEMPLATE_PROBE_RAPID_BASE,
	TEMPLATE_PROBE_RAPID_FULL
};

struct template_probe {
	struct http_request       *request;
	struct dstr                response;
	struct app                *app;
	struct dstr                rapid_base;
	size_t                     index;
	enum template_probe_stage  stage;
	int                        done;
};

struct model_run {
	struct http_request *request;
	struct sse           sse;
	struct dstr          answer;
	struct dstr          reasoning;
	struct dstr          tail;
	struct app          *app;
	size_t               index;
	int                  error;
	int                  done;
	int                  saw_done;
};

enum rapid_stage {
	RAPID_IDLE,
	RAPID_TEMPLATE,
	RAPID_THINK,
	RAPID_ANSWER
};

struct rapid_model {
	struct http_request *request;
	struct sse           sse;
	struct dstr          response;
	struct dstr          base;
	struct dstr          emitted;
	struct dstr          chunk;
	struct dstr          answer;
	struct dstr          tail;
	struct app          *app;
	size_t               index;
	uint32_t             tokens;
	uint32_t             requested;
	int                  error;
	int                  done;
	int                  saw_stop;
	int                  stop_type_seen;
	int                  hit_limit;
};

struct app {
	struct template_probe  probe[2];
	struct model_run       model[2];
	struct rapid_model     rapid[2];
	struct app_cfg         cfg;
	struct loop_ref        stdin_ref;
	struct list            history;
	struct list            prompts;
	struct turn           *pending;
	struct http           *http;
	struct loop            loop;
	struct dstr            stdin_buf;
	struct dstr            bearer;
	struct dstr            generation_tail[2];
	struct dstr            reasoning_tail[2];
	struct dstr            transition[2];
	long                   crosstalk_left;
	uint32_t               rapid_used;
	uint32_t               rapid_round;
	enum rapid_stage       rapid_stage;
	int                    stdin_flags;
	int                    stdin_eof;
	int                    exit_after_turn;
	int                    error;
	int                    template_ready;
};

static char const probe_reasoning_old[] =
	"__TWINCEPTION_REASONING_PROBE_OLD_196F89C7__";
static char const probe_reasoning_new[] =
	"__TWINCEPTION_REASONING_PROBE_NEW_5A2E4D31__";
static char const probe_rapid_reasoning[] =
	"__TWINCEPTION_RAPID_REASONING_8E6A09B3__";
static char const probe_rapid_answer[] =
	"__TWINCEPTION_RAPID_ANSWER_71C4F052__";

static void app_advance (struct app *app);
static void app_maybe_finish (struct app *app);
static void app_turn_committed (struct app *app, struct turn *turn);
static void rapid_maybe_advance (struct app *app);
static size_t rapid_write (char const *, size_t, void *);
static void rapid_done (CURLcode, long, char const *, void *);
static void template_probe_done (CURLcode, long, char const *, void *);

static void
turn_destroy (struct turn **p_turn)
{
	if (!p_turn || !*p_turn)
		return;
	struct turn *turn = *p_turn;
	*p_turn = nullptr;
	for (size_t i = 0; i < ARRAY_SIZE(turn->user); ++i) {
		dstr_cleanup(&turn->reasoning[i]);
		dstr_cleanup(&turn->answer[i]);
		dstr_cleanup(&turn->user[i]);
	}
	free(turn);
}

static struct turn *
turn_create (struct dstr const *user_a,
             struct dstr const *user_b)
{
	struct turn *turn = malloc(sizeof *turn);
	if (!turn)
		return nullptr;
	*turn = (struct turn){
		.user = { DSTR_INIT, DSTR_INIT },
		.answer = { DSTR_INIT, DSTR_INIT },
		.reasoning = { DSTR_INIT, DSTR_INIT }
	};
	list_init(&turn->hook);

	if (!dstr_cat_n(&turn->user[0], dstr_get(user_a), dstr_len(user_a)) ||
	    !dstr_cat_n(&turn->user[1], dstr_get(user_b), dstr_len(user_b))) {
		turn_destroy(&turn);
		return nullptr;
	}
	return turn;
}

static void
prompt_destroy (struct prompt **p_prompt)
{
	if (!p_prompt || !*p_prompt)
		return;
	struct prompt *prompt = *p_prompt;
	*p_prompt = nullptr;
	dstr_cleanup(&prompt->text);
	free(prompt);
}

static struct prompt *
prompt_create (char const *text,
               size_t      len)
{
	struct prompt *prompt = malloc(sizeof *prompt);
	if (!prompt)
		return nullptr;
	*prompt = (struct prompt){ .text = DSTR_INIT };
	list_init(&prompt->hook);

	if (!dstr_cat_n(&prompt->text, text, len)) {
		prompt_destroy(&prompt);
		return nullptr;
	}
	return prompt;
}

static int
json_add_message_key (struct json_object *messages,
                      char const         *role,
                      char const         *content,
                      char const         *reasoning,
                      char const         *reasoning_key)
{
	struct json_object *msg = json_object_new_object();
	struct json_object *role_obj = json_object_new_string(role);
	struct json_object *content_obj = json_object_new_string(content);
	struct json_object *reasoning_obj = reasoning
		? json_object_new_string(reasoning) : nullptr;
	if (!msg || !role_obj || !content_obj || (reasoning && !reasoning_obj)) {
		json_object_put(reasoning_obj);
		json_object_put(content_obj);
		json_object_put(role_obj);
		json_object_put(msg);
		return ENOMEM;
	}

	json_object_object_add(msg, "role", role_obj);
	json_object_object_add(msg, "content", content_obj);
	if (reasoning)
		json_object_object_add(msg, reasoning_key, reasoning_obj);

	if (json_object_array_add(messages, msg)) {
		json_object_put(msg);
		return ENOMEM;
	}
	return 0;
}

static int
json_add_value (struct json_object *object,
                char const         *key,
                struct json_object *value)
{
	if (!value)
		return ENOMEM;
	json_object_object_add(object, key, value);
	return 0;
}

static int
json_add_message (struct json_object *messages,
                  char const         *role,
                  char const         *content,
                  char const         *reasoning)
{
	return json_add_message_key(messages, role, content, reasoning,
	                            "reasoning_content");
}

static int
json_add_deepinfra_reasoning (struct json_object *root)
{
	struct json_object *reasoning = json_object_new_object();
	struct json_object *kwargs = json_object_new_object();
	struct json_object *enabled = json_object_new_boolean(1);
	struct json_object *enable_thinking = json_object_new_boolean(1);
	struct json_object *preserve_thinking = json_object_new_boolean(1);
	if (!reasoning || !kwargs || !enabled || !enable_thinking ||
	    !preserve_thinking) {
		json_object_put(preserve_thinking);
		json_object_put(enable_thinking);
		json_object_put(enabled);
		json_object_put(kwargs);
		json_object_put(reasoning);
		return ENOMEM;
	}

	json_object_object_add(reasoning, "enabled", enabled);
	json_object_object_add(kwargs, "enable_thinking", enable_thinking);
	json_object_object_add(kwargs, "preserve_thinking", preserve_thinking);
	json_object_object_add(root, "reasoning", reasoning);
	json_object_object_add(root, "chat_template_kwargs", kwargs);
	return 0;
}

/** POST @p body and release it; a null @p body, which failed to build, is ENOMEM. */
static int
model_post_json (struct app          *app,
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

static void
app_fail (struct app *app,
          int         error)
{
	if (!app->error)
		app->error = error;
	loop_exit(&app->loop);
}

/**
 * Append @p chat_url to @p url, with @p endpoint replacing its chat-completions
 * path; EINVAL if it has none.
 */
static int
endpoint_url (struct dstr *url,
              char const  *chat_url,
              char const  *endpoint)
{
	static char const suffix[] = "/v1/chat/completions";
	size_t len = strlen(chat_url);
	size_t suffix_len = sizeof suffix - 1;
	if (len < suffix_len || memcmp(chat_url + len - suffix_len,
	                               suffix, suffix_len))
		return EINVAL;

	return dstr_cat_n(url, chat_url, len - suffix_len) &&
	       dstr_cat(url, endpoint) ? 0 : ENOMEM;
}

static struct dstr const *
turn_answer_for (struct app const  *app,
                 struct turn const *turn,
                 size_t             model)
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

static int
json_add_history (struct json_object *messages,
                  struct app const   *app,
                  size_t              model)
{
	struct list *node;
	list_foreach (node, &app->history) {
		struct turn *turn = container_of(node, struct turn, hook);
		if (json_add_message(messages, "user", dstr_get(&turn->user[model]),
		                     nullptr) ||
		    json_add_message(messages, "assistant",
		                     dstr_get(turn_answer_for(app, turn, model)),
		                     dstr_get(&turn->reasoning[1 - model])))
			return ENOMEM;
	}
	return 0;
}

static struct json_object *
template_probe_body (struct app const *app,
                     enum template_probe_stage stage)
{
	struct json_object *root = json_object_new_object();
	struct json_object *messages = json_object_new_array();
	if (!root || !messages) {
		json_object_put(root);
		json_object_put(messages);
		return nullptr;
	}
	json_object_object_add(root, "messages", messages);

	int error;
	if (stage == TEMPLATE_PROBE_RAPID_BASE) {
		error = (app->cfg.system &&
		         json_add_message(messages, "system", app->cfg.system, nullptr)) ||
		        json_add_message(messages, "user", "rapid template probe", nullptr);
	} else if (stage == TEMPLATE_PROBE_RAPID_FULL) {
		error = (app->cfg.system &&
		         json_add_message(messages, "system", app->cfg.system, nullptr)) ||
		        json_add_message(messages, "user", "rapid template probe", nullptr) ||
		        json_add_message(messages, "assistant", probe_rapid_answer,
		                         probe_rapid_reasoning);
	} else {
		error = (app->cfg.system &&
		         json_add_message(messages, "system", app->cfg.system, nullptr)) ||
		        json_add_message(messages, "user", "template probe 0", nullptr) ||
		        json_add_message(messages, "assistant", "template probe answer 0",
		                         probe_reasoning_old) ||
		        json_add_message(messages, "user", "template probe 1", nullptr) ||
		        json_add_message(messages, "assistant", "template probe answer 1",
		                         probe_reasoning_new) ||
		        json_add_message(messages, "user", "template probe 2", nullptr);
	}
	if (error) {
		json_object_put(root);
		return nullptr;
	}
	return root;
}

static size_t
template_probe_write (char const *data,
                      size_t      len,
                      void       *user)
{
	struct template_probe *probe = user;
	return dstr_cat_n(&probe->response, data, len) ? len : 0;
}

/** Append the prompt from the probe's response to @p prompt. */
static int
template_probe_prompt (struct template_probe *probe,
                       struct dstr           *prompt)
{
	if (!dstr_len(&probe->response)) {
		fprintf(stderr, "model %c template check failed: empty response\n",
		        (int)('A' + probe->index));
		return EPROTO;
	}

	struct json_object *root = json_tokener_parse(dstr_get(&probe->response));
	if (!root) {
		fprintf(stderr, "model %c template check failed: invalid JSON response\n",
		        (int)('A' + probe->index));
		return EPROTO;
	}

	struct json_object *value;
	if (!json_object_object_get_ex(root, "prompt", &value) ||
	    json_object_get_type(value) != json_type_string) {
		fprintf(stderr,
		        "model %c template check failed: response has no prompt string\n",
		        (int)('A' + probe->index));
		json_object_put(root);
		return EPROTO;
	}

	int error = dstr_cat(prompt, json_object_get_string(value)) ? 0 : ENOMEM;
	json_object_put(root);
	return error;
}

static int
template_probe_validate_history (struct template_probe *probe,
                                 char const             *prompt)
{
	if (!strstr(prompt, probe_reasoning_new)) {
		fprintf(stderr,
		        "model %c template check failed: reasoning_content is absent "
		        "from /apply-template output\n",
		        (int)('A' + probe->index));
		return EPROTO;
	}

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

static size_t
common_prefix_len (char const *a,
                   char const *b)
{
	size_t n = 0;
	while (a[n] && b[n] && a[n] == b[n])
		++n;
	return n;
}

static int
template_probe_validate_rapid (struct template_probe *probe,
                               char const             *prompt)
{
	char const *base = dstr_get(&probe->rapid_base);
	char const *reasoning = strstr(prompt, probe_rapid_reasoning);
	char const *answer = reasoning
		? strstr(reasoning + sizeof probe_rapid_reasoning - 1, probe_rapid_answer)
		: nullptr;
	if (!reasoning || !answer) {
		fprintf(stderr,
		        "model %c rapid template check failed: cannot locate generation, "
		        "reasoning and answer prefills in /apply-template output\n",
		        (int)('A' + probe->index));
		return EPROTO;
	}

	size_t common = common_prefix_len(base, prompt);
	size_t base_len = dstr_len(&probe->rapid_base);
	size_t reasoning_off = (size_t)(reasoning - prompt);
	if (common > reasoning_off) {
		fprintf(stderr,
		        "model %c rapid template check failed: generation prompt overlaps "
		        "reasoning payload\n",
		        (int)('A' + probe->index));
		return EPROTO;
	}

	char const *begin = reasoning + sizeof probe_rapid_reasoning - 1;
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
template_probe_validate (struct template_probe *probe)
{
	dstr_auto prompt = DSTR_INIT;
	int error = template_probe_prompt(probe, &prompt);
	if (error)
		return error;

	return probe->app->cfg.rapid_quantum
		? template_probe_validate_rapid(probe, dstr_get(&prompt))
		: template_probe_validate_history(probe, dstr_get(&prompt));
}

static int
template_probe_start (struct template_probe *probe)
{
	struct app *app = probe->app;
	dstr_auto url = DSTR_INIT;
	int error = endpoint_url(&url, app->cfg.url[probe->index], "/apply-template");
	if (error == EINVAL)
		fprintf(stderr,
		        "cannot derive /apply-template from model %c URL%s\n",
		        (int)('A' + probe->index),
		        app->cfg.rapid_quantum
		        ? "; rapid mode requires llama-server-compatible endpoints"
		        : "; use -P to skip template validation");
	if (error)
		return error;

	dstr_clear(&probe->response);
	return model_post_json(app, probe->index, dstr_get(&url),
	                       template_probe_body(app, probe->stage),
	                       template_probe_write, template_probe_done, probe,
	                       &probe->request);
}

static void
template_probe_done (CURLcode    result,
                     long        status,
                     char const *curl_error,
                     void       *user)
{
	struct template_probe *probe = user;
	struct app *app = probe->app;
	probe->request = nullptr;

	if (result != CURLE_OK || status < 200 || status >= 300) {
		fprintf(stderr, "model %c template check failed: HTTP %ld, curl=%s%s%s\n",
		        (int)('A' + probe->index), status, curl_easy_strerror(result),
		        curl_error && *curl_error ? ": " : "",
		        curl_error && *curl_error ? curl_error : "");
		app_fail(app, EIO);
		return;
	}

	if (probe->stage == TEMPLATE_PROBE_RAPID_BASE) {
		/* The base stage runs once, so rapid_base starts out empty. */
		int error = template_probe_prompt(probe, &probe->rapid_base);
		if (error) {
			app_fail(app, error);
			return;
		}
		probe->stage = TEMPLATE_PROBE_RAPID_FULL;
		error = template_probe_start(probe);
		if (error)
			app_fail(app, error);
		return;
	}

	int error = template_probe_validate(probe);
	if (error) {
		app_fail(app, error);
		return;
	}
	probe->done = 1;
	if (!app->probe[0].done || !app->probe[1].done)
		return;

	app->template_ready = 1;
	app_advance(app);
}

static int
app_start_template_probes (struct app *app)
{
	if (!app->cfg.template_check) {
		if (app->cfg.rapid_quantum &&
		    (app->cfg.provider[0] == APP_PROVIDER_LLAMA ||
		     app->cfg.provider[1] == APP_PROVIDER_LLAMA)) {
			fputs("rapid mode requires template transition discovery; "
			      "-P cannot be used with --rapid for llama providers\n", stderr);
			return EINVAL;
		}
		app->template_ready = 1;
		return 0;
	}

	for (size_t i = 0; i < ARRAY_SIZE(app->probe); ++i) {
		struct template_probe *probe = &app->probe[i];
		if (app->cfg.provider[i] == APP_PROVIDER_DEEPINFRA) {
			probe->done = 1;
			continue;
		}
		probe->stage = app->cfg.rapid_quantum
			? TEMPLATE_PROBE_RAPID_BASE : TEMPLATE_PROBE_HISTORY;
		int error = template_probe_start(probe);
		if (error)
			return error;
	}
	if (app->probe[0].done && app->probe[1].done)
		app->template_ready = 1;
	return 0;
}

static struct json_object *
chat_request_body (struct app const *app,
                   size_t            model,
                   char const       *user)
{
	struct json_object *root = json_object_new_object();
	struct json_object *messages = json_object_new_array();
	if (!root || !messages) {
		json_object_put(root);
		json_object_put(messages);
		return nullptr;
	}

	json_object_object_add(root, "messages", messages);
	if (json_add_value(root, "stream", json_object_new_boolean(1)))
		goto fail;
	if (app->cfg.provider[model] == APP_PROVIDER_LLAMA &&
	    json_add_value(root, "reasoning_format", json_object_new_string("deepseek")))
		goto fail;
	if (app->cfg.provider[model] == APP_PROVIDER_DEEPINFRA &&
	    json_add_deepinfra_reasoning(root))
		goto fail;
	if (app->cfg.model[model])
		if (json_add_value(root, "model",
		                   json_object_new_string(app->cfg.model[model])))
			goto fail;
	if (app->cfg.temperature >= 0)
		if (json_add_value(root, "temperature",
		                   json_object_new_double(app->cfg.temperature)))
			goto fail;
	if (app->cfg.max_tokens > 0)
		if (json_add_value(root, "max_tokens",
		                   json_object_new_int64(app->cfg.max_tokens)))
			goto fail;

	if ((app->cfg.system &&
	     json_add_message(messages, "system", app->cfg.system, nullptr)) ||
	    json_add_history(messages, app, model) ||
	    json_add_message(messages, "user", user, nullptr))
		goto fail;
	return root;

fail:
	json_object_put(root);
	return nullptr;
}

static struct json_object *
template_turn_body (struct app const *app,
                    size_t            model,
                    char const       *user)
{
	struct json_object *root = json_object_new_object();
	struct json_object *messages = json_object_new_array();
	if (!root || !messages) {
		json_object_put(root);
		json_object_put(messages);
		return nullptr;
	}
	json_object_object_add(root, "messages", messages);
	if (app->cfg.model[model])
		json_object_object_add(root, "model",
		                       json_object_new_string(app->cfg.model[model]));

	if ((app->cfg.system &&
	     json_add_message(messages, "system", app->cfg.system, nullptr)) ||
	    json_add_history(messages, app, model) ||
	    json_add_message(messages, "user", user, nullptr)) {
		json_object_put(root);
		return nullptr;
	}
	return root;
}

static void
model_tail_append (struct dstr *tail,
                   char const  *data,
                   size_t       len)
{
	enum { MAX_TAIL = 8192 };
	if (len >= MAX_TAIL) {
		dstr_clear(tail);
		dstr_cat_n(tail, data + len - MAX_TAIL, MAX_TAIL);
		return;
	}

	size_t tail_len = dstr_len(tail);
	if (tail_len + len > MAX_TAIL)
		dstr_erase(tail, 0, tail_len + len - MAX_TAIL);
	dstr_cat_n(tail, data, len);
}

static int
model_sse_event (char const *data,
                 size_t      len,
                 void       *user)
{
	struct model_run *model = user;
	if (len == 6 && !memcmp(data, "[DONE]", 6)) {
		model->saw_done = 1;
		return 0;
	}

	struct json_tokener *tok = json_tokener_new();
	if (!tok)
		return ENOMEM;
	struct json_object *root = json_tokener_parse_ex(tok, data, (int)len);
	enum json_tokener_error je = json_tokener_get_error(tok);
	json_tokener_free(tok);
	if (je != json_tokener_success || !root) {
		json_object_put(root);
		return EPROTO;
	}

	struct json_object *error;
	if (json_object_object_get_ex(root, "error", &error)) {
		json_object_put(root);
		return EREMOTEIO;
	}

	struct json_object *choices;
	struct json_object *choice;
	struct json_object *delta;
	if (!json_object_object_get_ex(root, "choices", &choices) ||
	    json_object_get_type(choices) != json_type_array ||
	    !json_object_array_length(choices) ||
	    !(choice = json_object_array_get_idx(choices, 0)) ||
	    !json_object_object_get_ex(choice, "delta", &delta)) {
		json_object_put(root);
		return 0;
	}

	struct json_object *value;
	int e = 0;
	if (json_object_object_get_ex(delta, "reasoning_content", &value) &&
	    json_object_get_type(value) == json_type_string) {
		char const *str = json_object_get_string(value);
		size_t n = (size_t)json_object_get_string_len(value);
		e = dstr_cat_n(&model->reasoning, str, n) ? 0 : ENOMEM;
	} else if (json_object_object_get_ex(delta, "reasoning", &value) &&
	           json_object_get_type(value) == json_type_string) {
		char const *str = json_object_get_string(value);
		size_t n = (size_t)json_object_get_string_len(value);
		e = dstr_cat_n(&model->reasoning, str, n) ? 0 : ENOMEM;
	}

	if (!e && json_object_object_get_ex(delta, "content", &value) &&
	    json_object_get_type(value) == json_type_string) {
		char const *str = json_object_get_string(value);
		size_t n = (size_t)json_object_get_string_len(value);
		e = dstr_cat_n(&model->answer, str, n) ? 0 : ENOMEM;
	}

	json_object_put(root);
	return e;
}

static size_t
model_write (char const *data,
             size_t      len,
             void       *user)
{
	struct model_run *model = user;
	model_tail_append(&model->tail, data, len);
	int e = sse_feed(&model->sse, data, len);
	if (e) {
		model->error = e;
		return 0;
	}
	return len;
}

static void
model_done (CURLcode    result,
            long        status,
            char const *curl_error,
            void       *user)
{
	struct model_run *model = user;
	model->request = nullptr;
	model->done = 1;

	if (!model->error) {
		int e = sse_finish(&model->sse);
		if (e)
			model->error = e;
	}

	if (!model->error &&
	    (result != CURLE_OK || status < 200 || status >= 300 ||
	     !model->saw_done))
		model->error = result == CURLE_OK ? EPROTO : EIO;

	if (model->error) {
		fprintf(stderr, "model %c failed: HTTP %ld, curl=%s%s%s\n",
		        (int)('A' + model->index), status,
		        curl_easy_strerror(result),
		        curl_error && *curl_error ? ": " : "",
		        curl_error && *curl_error ? curl_error : "");
		int tail_len = (int)dstr_len(&model->tail);
		if (tail_len)
			fprintf(stderr, "model %c response tail:\n%.*s\n",
			        (int)('A' + model->index),
			        tail_len, dstr_get(&model->tail));
	}

	app_maybe_finish(model->app);
}

static void
model_reset (struct model_run *model)
{
	sse_fini(&model->sse);
	dstr_cleanup(&model->tail);
	dstr_cleanup(&model->reasoning);
	dstr_cleanup(&model->answer);
	model->error = 0;
	model->done = 0;
	model->saw_done = 0;
	model->sse = sse(model_sse_event, model);
}

static int
app_start_chat (struct app *app)
{
	for (size_t i = 0; i < ARRAY_SIZE(app->model); ++i) {
		struct model_run *model = &app->model[i];
		model_reset(model);
		struct json_object *body =
			chat_request_body(app, i, dstr_get(&app->pending->user[i]));
		int e = model_post_json(app, i, app->cfg.url[i], body,
		                        model_write, model_done, model,
		                        &model->request);
		if (e)
			return e;
	}
	return 0;
}

static void
rapid_reset_request (struct rapid_model *model)
{
	sse_fini(&model->sse);
	dstr_clear(&model->chunk);
	dstr_clear(&model->tail);
	model->tokens = 0;
	model->requested = 0;
	model->error = 0;
	model->done = 0;
	model->saw_stop = 0;
	model->stop_type_seen = 0;
	model->hit_limit = 0;
}

static void
rapid_reset_turn (struct rapid_model *model)
{
	sse_fini(&model->sse);
	dstr_cleanup(&model->response);
	dstr_cleanup(&model->base);
	dstr_cleanup(&model->emitted);
	dstr_cleanup(&model->chunk);
	dstr_cleanup(&model->answer);
	dstr_cleanup(&model->tail);
	model->tokens = 0;
	model->requested = 0;
	model->error = 0;
	model->done = 0;
	model->saw_stop = 0;
	model->stop_type_seen = 0;
	model->hit_limit = 0;
}

static size_t
rapid_template_write (char const *data,
                      size_t      len,
                      void       *user)
{
	struct rapid_model *model = user;
	return dstr_cat_n(&model->response, data, len) ? len : 0;
}

static int
rapid_template_parse (struct rapid_model *model)
{
	struct json_object *root = json_tokener_parse(dstr_get(&model->response));
	if (!root)
		return EPROTO;

	struct json_object *value;
	int error = 0;
	if (!json_object_object_get_ex(root, "prompt", &value) ||
	    json_object_get_type(value) != json_type_string) {
		error = EPROTO;
	} else {
		char const *str = json_object_get_string(value);
		size_t len = (size_t)json_object_get_string_len(value);
		error = dstr_cat_n(&model->base, str, len) ? 0 : ENOMEM;
	}
	json_object_put(root);
	return error;
}

static void
rapid_template_done (CURLcode    result,
                     long        status,
                     char const *curl_error,
                     void       *user)
{
	struct rapid_model *model = user;
	model->request = nullptr;
	model->done = 1;

	if (result != CURLE_OK || status < 200 || status >= 300) {
		model->error = EIO;
		fprintf(stderr,
		        "model %c turn template failed: HTTP %ld, curl=%s%s%s\n",
		        (int)('A' + model->index), status, curl_easy_strerror(result),
		        curl_error && *curl_error ? ": " : "",
		        curl_error && *curl_error ? curl_error : "");
	} else {
		model->error = rapid_template_parse(model);
		if (model->error)
			fprintf(stderr, "model %c turn template returned invalid JSON\n",
			        (int)('A' + model->index));
	}

	rapid_maybe_advance(model->app);
}

static int
rapid_sse_event (char const *data,
                 size_t      len,
                 void       *user)
{
	struct rapid_model *model = user;
	if (len == 6 && !memcmp(data, "[DONE]", 6)) {
		model->saw_stop = 1;
		return 0;
	}

	struct json_tokener *tok = json_tokener_new();
	if (!tok)
		return ENOMEM;
	struct json_object *root = json_tokener_parse_ex(tok, data, (int)len);
	enum json_tokener_error je = json_tokener_get_error(tok);
	json_tokener_free(tok);
	if (je != json_tokener_success || !root) {
		json_object_put(root);
		return EPROTO;
	}

	struct json_object *error;
	if (json_object_object_get_ex(root, "error", &error)) {
		json_object_put(root);
		return EREMOTEIO;
	}

	if (model->app->cfg.provider[model->index] == APP_PROVIDER_DEEPINFRA) {
		struct json_object *choices;
		struct json_object *choice;
		struct json_object *delta;
		if (!json_object_object_get_ex(root, "choices", &choices) ||
		    json_object_get_type(choices) != json_type_array ||
		    !json_object_array_length(choices) ||
		    !(choice = json_object_array_get_idx(choices, 0))) {
			json_object_put(root);
			return 0;
		}

		int e = 0;
		struct json_object *value = nullptr;
		if (json_object_object_get_ex(choice, "delta", &delta)) {
			if (model->app->rapid_stage == RAPID_ANSWER)
				json_object_object_get_ex(delta, "content", &value);
			else if (!json_object_object_get_ex(delta, "reasoning_content", &value) &&
			         !json_object_object_get_ex(delta, "reasoning", &value))
				json_object_object_get_ex(delta, "content", &value);

			if (value && json_object_get_type(value) == json_type_string) {
				char const *str = json_object_get_string(value);
				size_t n = (size_t)json_object_get_string_len(value);
				struct dstr *out = model->app->rapid_stage == RAPID_ANSWER
					? &model->answer : &model->chunk;
				e = dstr_cat_n(out, str, n) ? 0 : ENOMEM;
			}
		}
		if (!e && json_object_object_get_ex(choice, "finish_reason", &value) &&
		    json_object_get_type(value) == json_type_string) {
			model->stop_type_seen = 1;
			model->hit_limit = !strcmp(json_object_get_string(value), "length");
			if (model->hit_limit)
				model->tokens = model->requested;
		}
		json_object_put(root);
		return e;
	}

	struct json_object *value;
	int e = 0;
	if (json_object_object_get_ex(root, "content", &value) &&
	    json_object_get_type(value) == json_type_string) {
		char const *str = json_object_get_string(value);
		size_t n = (size_t)json_object_get_string_len(value);
		struct dstr *out = model->app->rapid_stage == RAPID_ANSWER
			? &model->answer : &model->chunk;
		e = dstr_cat_n(out, str, n) ? 0 : ENOMEM;
	}

	if (!e && model->app->rapid_stage == RAPID_THINK &&
	    json_object_object_get_ex(root, "tokens", &value) &&
	    json_object_get_type(value) == json_type_array) {
		size_t n = json_object_array_length(value);
		if (n > UINT32_MAX - model->tokens)
			e = EOVERFLOW;
		else
			model->tokens += (uint32_t)n;
	}

	if (!e && json_object_object_get_ex(root, "stop_type", &value) &&
	    json_object_get_type(value) == json_type_string) {
		model->stop_type_seen = 1;
		model->hit_limit = !strcmp(json_object_get_string(value), "limit");
	}

	if (!e && json_object_object_get_ex(root, "stop", &value) &&
	    json_object_get_boolean(value))
		model->saw_stop = 1;

	json_object_put(root);
	return e;
}

static struct json_object *
deepinfra_rapid_body (struct app const  *app,
                      size_t             model,
                      struct dstr const *foreign,
                      long               n_predict)
{
	struct json_object *root = json_object_new_object();
	struct json_object *messages = json_object_new_array();
	if (!root || !messages) {
		json_object_put(root);
		json_object_put(messages);
		return nullptr;
	}
	json_object_object_add(root, "messages", messages);
	if (json_add_value(root, "stream", json_object_new_boolean(1)) ||
	    json_add_value(root, "model", json_object_new_string(app->cfg.model[model])) ||
	    json_add_value(root, "continue_final_message", json_object_new_boolean(1)) ||
	    json_add_deepinfra_reasoning(root))
		goto fail;
	if (app->cfg.temperature >= 0)
		if (json_add_value(root, "temperature",
		                   json_object_new_double(app->cfg.temperature)))
			goto fail;
	if (n_predict > 0)
		if (json_add_value(root, "max_tokens",
		                   json_object_new_int64(n_predict)))
			goto fail;
	if (app->rapid_stage == RAPID_THINK)
		if (json_add_value(root, "stop", json_object_new_string("</think>")))
			goto fail;

	if ((app->cfg.system &&
	     json_add_message(messages, "system", app->cfg.system, nullptr)) ||
	    json_add_history(messages, app, model) ||
	    json_add_message(messages, "user", dstr_get(&app->pending->user[model]),
	                     nullptr))
		goto fail;

	struct dstr prefill = dstr("<think>\n");
	int e = ENOMEM;
	if (dstr_cat_n(&prefill, dstr_get(foreign), dstr_len(foreign)) &&
	    (app->rapid_stage != RAPID_ANSWER ||
	     dstr_cat_n(&prefill, "</think>\n", sizeof "</think>\n" - 1)))
		e = json_add_message(messages, "assistant", dstr_get(&prefill), nullptr);
	dstr_cleanup(&prefill);
	if (e)
		goto fail;
	return root;

fail:
	json_object_put(root);
	return nullptr;
}

static int
rapid_start_deepinfra (struct app         *app,
                       struct rapid_model *model,
                       struct dstr const  *foreign,
                       long                n_predict)
{
	rapid_reset_request(model);
	model->requested = n_predict > 0 ? (uint32_t)n_predict : 0;
	model->sse = sse(rapid_sse_event, model);

	return model_post_json(app, model->index, app->cfg.url[model->index],
	                       deepinfra_rapid_body(app, model->index, foreign,
	                                            n_predict),
	                       rapid_write, rapid_done, model, &model->request);
}

static size_t
rapid_write (char const *data,
             size_t      len,
             void       *user)
{
	struct rapid_model *model = user;
	model_tail_append(&model->tail, data, len);
	int e = sse_feed(&model->sse, data, len);
	if (e) {
		model->error = e;
		return 0;
	}
	return len;
}

static void
rapid_done (CURLcode    result,
            long        status,
            char const *curl_error,
            void       *user)
{
	struct rapid_model *model = user;
	model->request = nullptr;
	model->done = 1;

	if (!model->error) {
		int e = sse_finish(&model->sse);
		if (e)
			model->error = e;
	}

	if (!model->error &&
	    (result != CURLE_OK || status < 200 || status >= 300 ||
	     !model->saw_stop))
		model->error = result == CURLE_OK ? EPROTO : EIO;

	if (model->error) {
		fprintf(stderr, "model %c rapid request failed: HTTP %ld, curl=%s%s%s\n",
		        (int)('A' + model->index), status,
		        curl_easy_strerror(result),
		        curl_error && *curl_error ? ": " : "",
		        curl_error && *curl_error ? curl_error : "");
		int tail_len = (int)dstr_len(&model->tail);
		if (tail_len)
			fprintf(stderr, "model %c response tail:\n%.*s\n",
			        (int)('A' + model->index),
			        tail_len, dstr_get(&model->tail));
	}

	rapid_maybe_advance(model->app);
}

static struct json_object *
rapid_body (struct app const  *app,
            size_t             model,
            struct dstr const *prompt,
            char const        *stop,
            long               n_predict)
{
	size_t prompt_len = dstr_len(prompt);
	if (prompt_len > INT_MAX)
		return nullptr;
	struct json_object *root = json_object_new_object();
	if (!root)
		return nullptr;

	if (json_add_value(root, "prompt",
	                   json_object_new_string_len(dstr_get(prompt),
	                                              (int)prompt_len)) ||
	    json_add_value(root, "stream", json_object_new_boolean(1)) ||
	    json_add_value(root, "cache_prompt", json_object_new_boolean(1))) {
		json_object_put(root);
		return nullptr;
	}
	if (stop) {
		struct json_object *stops = json_object_new_array();
		struct json_object *stop_obj = json_object_new_string(stop);
		if (!stops || !stop_obj) {
			json_object_put(stop_obj);
			json_object_put(stops);
			json_object_put(root);
			return nullptr;
		}
		if (json_object_array_add(stops, stop_obj)) {
			json_object_put(stop_obj);
			json_object_put(stops);
			json_object_put(root);
			return nullptr;
		}
		json_object_object_add(root, "stop", stops);
	}
	if (n_predict > 0)
		if (json_add_value(root, "n_predict", json_object_new_int64(n_predict)))
			goto fail;
	if (app->cfg.model[model])
		if (json_add_value(root, "model",
		                   json_object_new_string(app->cfg.model[model])))
			goto fail;
	if (app->cfg.temperature >= 0)
		if (json_add_value(root, "temperature",
		                   json_object_new_double(app->cfg.temperature)))
			goto fail;
	return root;

fail:
	json_object_put(root);
	return nullptr;
}

static int
rapid_start_raw (struct app         *app,
                 struct rapid_model *model,
                 struct dstr const  *foreign,
                 struct dstr const  *suffix,
                 long                n_predict)
{
	rapid_reset_request(model);
	model->requested = n_predict > 0 ? (uint32_t)n_predict : 0;
	model->sse = sse(rapid_sse_event, model);

	/* The probe stores a nonempty transition along with both tails. */
	struct dstr const *transition = &app->transition[model->index];
	if (!dstr_len(transition))
		return EPROTO;

	struct dstr const *generation_tail = &app->generation_tail[model->index];
	struct dstr const *reasoning_tail = &app->reasoning_tail[model->index];
	char const *base = dstr_get(&model->base);
	size_t base_len = dstr_len(&model->base);
	size_t generation_len = dstr_len(generation_tail);
	if (generation_len > base_len ||
	    memcmp(base + base_len - generation_len, dstr_get(generation_tail),
	           generation_len)) {
		fprintf(stderr,
		        "model %c rapid turn template no longer matches probed generation tail\n",
		        (int)('A' + model->index));
		return EPROTO;
	}

	dstr_auto prompt = DSTR_INIT;
	if (!dstr_cat_n(&prompt, base, base_len - generation_len) ||
	    !dstr_cat_n(&prompt, dstr_get(reasoning_tail), dstr_len(reasoning_tail)) ||
	    !dstr_cat_n(&prompt, dstr_get(foreign), dstr_len(foreign)) ||
	    !dstr_cat_n(&prompt, dstr_get(suffix), dstr_len(suffix)))
		return ENOMEM;

	dstr_auto url = DSTR_INIT;
	int e = endpoint_url(&url, app->cfg.url[model->index], "/completion");
	if (e)
		return e;

	struct json_object *body =
		rapid_body(app, model->index, &prompt,
		           app->rapid_stage == RAPID_THINK ? dstr_get(transition) : nullptr,
		           n_predict);
	return model_post_json(app, model->index, dstr_get(&url), body,
	                       rapid_write, rapid_done, model, &model->request);
}

static int
rapid_start_quantum (struct app *app)
{
	uint32_t remaining = app->cfg.rapid_budget - app->rapid_used;
	uint32_t n = remaining < app->cfg.rapid_quantum
		? remaining : app->cfg.rapid_quantum;
	if (!n)
		return EINVAL;

	app->rapid_stage = RAPID_THINK;
	++app->rapid_round;
	for (size_t i = 0; i < ARRAY_SIZE(app->rapid); ++i) {
		int e = app->cfg.provider[i] == APP_PROVIDER_DEEPINFRA
			? rapid_start_deepinfra(app, &app->rapid[i],
			                   &app->rapid[1 - i].emitted, (long)n)
			: rapid_start_raw(app, &app->rapid[i],
			                  &app->rapid[1 - i].emitted, nullptr, (long)n);
		if (e)
			return e;
	}
	return 0;
}

static int
rapid_start_answers (struct app *app)
{
	app->rapid_stage = RAPID_ANSWER;
	for (size_t i = 0; i < ARRAY_SIZE(app->rapid); ++i) {
		int e = app->cfg.provider[i] == APP_PROVIDER_DEEPINFRA
			? rapid_start_deepinfra(app, &app->rapid[i],
			                   &app->rapid[1 - i].emitted, app->cfg.max_tokens)
			: rapid_start_raw(app, &app->rapid[i],
			                  &app->rapid[1 - i].emitted,
			                  &app->transition[i], app->cfg.max_tokens);
		if (e)
			return e;
	}
	return 0;
}

static void
rapid_print_quantum (struct app const *app)
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

static void
rapid_commit (struct app *app)
{
	struct turn *turn = app->pending;
	app->pending = nullptr;

	for (size_t i = 0; i < 2; ++i) {
		turn->answer[i] = dstr_move(&app->rapid[i].answer);
		turn->reasoning[i] = dstr_move(&app->rapid[i].emitted);
	}
	list_append(&app->history, &turn->hook);
	app->rapid_stage = RAPID_IDLE;
	app_turn_committed(app, turn);
}

static void
rapid_maybe_advance (struct app *app)
{
	if (!app->pending || !app->rapid[0].done || !app->rapid[1].done)
		return;

	if (app->rapid[0].error || app->rapid[1].error) {
		fprintf(stderr, "rapid paired turn discarded; history unchanged\n");
		turn_destroy(&app->pending);
		app->rapid_stage = RAPID_IDLE;
		app->crosstalk_left = 0;
		app_advance(app);
		return;
	}

	if (app->rapid_stage == RAPID_TEMPLATE) {
		int e = rapid_start_quantum(app);
		if (e)
			app_fail(app, e);
		return;
	}

	if (app->rapid_stage == RAPID_THINK) {
		rapid_print_quantum(app);
		int e = 0;
		for (size_t i = 0; i < 2 && !e; ++i)
			e = dstr_cat_n(&app->rapid[i].emitted,
			               dstr_get(&app->rapid[i].chunk),
			               dstr_len(&app->rapid[i].chunk)) ? 0 : ENOMEM;
		if (e) {
			app_fail(app, e);
			return;
		}

		uint32_t requested = app->rapid[0].requested;
		app->rapid_used += requested;
		int early = 0;
		for (size_t i = 0; i < 2; ++i) {
			struct rapid_model const *model = &app->rapid[i];
			early |= model->stop_type_seen
				? !model->hit_limit : model->tokens < requested;
		}
		if (early && app->cfg.debug)
			fprintf(stderr,
			        "rapid reasoning ended early at round %u; forcing answer transition\n",
			        app->rapid_round);

		if (early || app->rapid_used >= app->cfg.rapid_budget)
			e = rapid_start_answers(app);
		else
			e = rapid_start_quantum(app);
		if (e)
			app_fail(app, e);
		return;
	}

	if (app->rapid_stage == RAPID_ANSWER) {
		rapid_commit(app);
		return;
	}

	app_fail(app, EPROTO);
}

static int
app_start_rapid (struct app *app)
{
	app->rapid_stage = RAPID_TEMPLATE;
	app->rapid_used = 0;
	app->rapid_round = 0;

	for (size_t i = 0; i < ARRAY_SIZE(app->rapid); ++i) {
		struct rapid_model *model = &app->rapid[i];
		rapid_reset_turn(model);
		model->app = app;
		model->index = i;
		if (app->cfg.provider[i] == APP_PROVIDER_DEEPINFRA) {
			model->done = 1;
			continue;
		}

		dstr_auto url = DSTR_INIT;
		int e = endpoint_url(&url, app->cfg.url[i], "/apply-template");
		if (e)
			return e;
		e = model_post_json(app, i, dstr_get(&url),
		                    template_turn_body(app, i,
		                                       dstr_get(&app->pending->user[i])),
		                    rapid_template_write, rapid_template_done, model,
		                    &model->request);
		if (e)
			return e;
	}
	if (app->rapid[0].done && app->rapid[1].done)
		rapid_maybe_advance(app);
	return 0;
}

static int
app_start_prompt (struct app        *app,
                  struct dstr const *user_a,
                  struct dstr const *user_b,
                  int                human)
{
	struct turn *turn = turn_create(user_a, user_b);
	if (!turn)
		return ENOMEM;
	app->pending = turn;
	if (human)
		app->crosstalk_left = app->cfg.crosstalk_rounds;

	return app->cfg.rapid_quantum ? app_start_rapid(app) : app_start_chat(app);
}

static void
app_print_turn (struct app const  *app,
                struct turn const *turn)
{
	if (app->cfg.debug)
		printf("\n--- A reasoning -> B ---\n\033[1;35m%s\033[0m\n"
		       "\n--- B reasoning -> A ---\n\033[1;36m%s\033[0m\n",
		       dstr_get(&turn->reasoning[0]), dstr_get(&turn->reasoning[1]));

	printf("\n[A]\n\033[1;35m%s\033[0m\n\n[B]\n\033[1;36m%s\033[0m\n",
	       dstr_get(&turn->answer[0]), dstr_get(&turn->answer[1]));
	fflush(stdout);
}

static int
app_start_crosstalk (struct app *app,
                     struct turn *turn,
                     int         *started)
{
	*started = 0;
	if (app->exit_after_turn || !app->crosstalk_left)
		return 0;
	if (app->crosstalk_left > 0)
		--app->crosstalk_left;

	if (app->cfg.debug)
		fprintf(stderr, "crosstalk: A <- B, B <- A%s\n",
		        app->crosstalk_left < 0 ? " (unbounded)" : "");
	int e = app_start_prompt(app, &turn->answer[1], &turn->answer[0], 0);
	if (!e)
		*started = 1;
	return e;
}

static struct prompt *
app_pop_prompt (struct app *app)
{
	if (list_is_empty(&app->prompts))
		return nullptr;
	struct prompt *prompt = container_of(app->prompts.next, struct prompt, hook);
	list_del(&prompt->hook);
	return prompt;
}

static void
app_advance (struct app *app)
{
	if (!app->template_ready || app->pending)
		return;

	struct prompt *prompt = app_pop_prompt(app);
	if (prompt) {
		int e = app_start_prompt(app, &prompt->text, &prompt->text, 1);
		prompt_destroy(&prompt);
		if (e) {
			fprintf(stderr, "cannot start request pair: %s\n", strerror(e));
			app_fail(app, e);
		}
		return;
	}

	if (app->stdin_eof || app->exit_after_turn) {
		loop_exit(&app->loop);
		return;
	}

	fputs("> ", stdout);
	fflush(stdout);
}

static void
app_turn_committed (struct app *app,
                    struct turn *turn)
{
	app_print_turn(app, turn);

	int started;
	int e = app_start_crosstalk(app, turn, &started);
	if (e) {
		fprintf(stderr, "cannot start crosstalk round: %s\n", strerror(e));
		app_fail(app, e);
		return;
	}
	if (!started)
		app_advance(app);
}

static void
app_maybe_finish (struct app *app)
{
	if (!app->pending || app->cfg.rapid_quantum ||
	    !app->model[0].done || !app->model[1].done)
		return;

	struct turn *turn = app->pending;
	app->pending = nullptr;

	if (!app->model[0].error && !app->model[1].error) {
		for (size_t i = 0; i < 2; ++i) {
			turn->answer[i] = dstr_move(&app->model[i].answer);
			turn->reasoning[i] = dstr_move(&app->model[i].reasoning);
		}
		list_append(&app->history, &turn->hook);
		app_turn_committed(app, turn);
		return;
	}

	fprintf(stderr, "paired turn discarded; history unchanged\n");
	turn_destroy(&turn);
	app->crosstalk_left = 0;
	app_advance(app);
}

static int
app_queue_prompt (struct app *app,
                  char const *text,
                  size_t      len)
{
	while (len && (text[len - 1] == '\n' || text[len - 1] == '\r'))
		--len;
	if (!len)
		return 0;
	if (len == 5 && !memcmp(text, ":quit", 5)) {
		app->exit_after_turn = 1;
		app->crosstalk_left = 0;
		if (!app->pending)
			loop_exit(&app->loop);
		return 0;
	}
	if (len == 5 && !memcmp(text, ":stop", 5)) {
		app->crosstalk_left = 0;
		return 0;
	}

	struct prompt *prompt = prompt_create(text, len);
	if (!prompt)
		return ENOMEM;
	list_append(&app->prompts, &prompt->hook);
	return 0;
}

static int
app_stdin_read (struct app *app,
                uint32_t    events)
{
	char data[4096];

	if (events & (EPOLLHUP | EPOLLRDHUP))
		app->stdin_eof = 1;

	for (;;) {
		ssize_t n = read(STDIN_FILENO, data, sizeof data);
		if (n > 0) {
			if (!dstr_cat_n(&app->stdin_buf, data, (size_t)n))
				return ENOMEM;
		} else if (!n) {
			app->stdin_eof = 1;
			break;
		} else if (errno == EINTR) {
			continue;
		} else if (errno == EAGAIN || errno == EWOULDBLOCK) {
			break;
		} else {
			return errno;
		}
	}

	char const *input = dstr_get(&app->stdin_buf);
	size_t input_len = dstr_len(&app->stdin_buf);
	size_t used = 0;
	while (used < input_len) {
		char const *line = input + used;
		char const *nl = memchr(line, '\n', input_len - used);
		if (!nl)
			break;
		size_t len = (size_t)(nl - line);
		int e = app_queue_prompt(app, line, len);
		if (e)
			return e;
		used += len + 1;
	}
	dstr_erase(&app->stdin_buf, 0, used);

	size_t rest = dstr_len(&app->stdin_buf);
	if (app->stdin_eof && rest) {
		int e = app_queue_prompt(app, dstr_get(&app->stdin_buf), rest);
		if (e)
			return e;
		dstr_clear(&app->stdin_buf);
	}

	app_advance(app);
	return 0;
}

static void
app_stdin_event (struct loop     *loop,
                 struct loop_ref *ref,
                 uint32_t         events)
{
	(void)loop;
	struct app *app = ref->owner;
	int e = app_stdin_read(app, events);
	if (e)
		app_fail(app, e);
}

static struct template_probe
template_probe (struct app *app,
                size_t      index)
{
	return (struct template_probe){
		.response = DSTR_INIT,
		.app = app,
		.rapid_base = DSTR_INIT,
		.index = index
	};
}

static struct model_run
model_run (struct app *app,
           size_t      index)
{
	return (struct model_run){
		.sse = sse(model_sse_event, &app->model[index]),
		.answer = DSTR_INIT,
		.reasoning = DSTR_INIT,
		.tail = DSTR_INIT,
		.app = app,
		.index = index
	};
}

static struct rapid_model
rapid_model (struct app *app,
             size_t      index)
{
	return (struct rapid_model){
		.sse = sse(rapid_sse_event, &app->rapid[index]),
		.response = DSTR_INIT,
		.base = DSTR_INIT,
		.emitted = DSTR_INIT,
		.chunk = DSTR_INIT,
		.answer = DSTR_INIT,
		.tail = DSTR_INIT,
		.app = app,
		.index = index
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
		.generation_tail = { DSTR_INIT, DSTR_INIT },
		.reasoning_tail = { DSTR_INIT, DSTR_INIT },
		.transition = { DSTR_INIT, DSTR_INIT },
		.stdin_flags = -1
	};
	return ret;
}

static int
app_init (struct app *app)
{
	for (size_t i = 0; i < ARRAY_SIZE(app->probe); ++i) {
		app->probe[i] = template_probe(app, i);
		app->model[i] = model_run(app, i);
		app->rapid[i] = rapid_model(app, i);
	}
	list_init(&app->history);
	list_init(&app->prompts);
	struct app_cfg const *cfg = &app->cfg;

	int e = loop_init(&app->loop);
	if (e)
		return e;

	if (cfg->deepinfra_key &&
	    !dstr_printf(&app->bearer, "Authorization: Bearer %s",
	                 cfg->deepinfra_key))
		return ENOMEM;

	app->http = http_create(&app->loop, nullptr, &e);
	if (!app->http)
		return e;

	if (cfg->prompt) {
		app->stdin_eof = 1;
		return app_queue_prompt(app, cfg->prompt, strlen(cfg->prompt));
	}

	app->stdin_flags = fcntl(STDIN_FILENO, F_GETFL);
	if (app->stdin_flags < 0)
		return errno;
	if (fcntl(STDIN_FILENO, F_SETFL, app->stdin_flags | O_NONBLOCK))
		return errno;

	e = loop_watch(&app->loop, &app->stdin_ref, STDIN_FILENO,
	               EPOLLIN | EPOLLRDHUP, app_stdin_event, app);
	if (e)
		return e;
	return 0;
}

static void
app_fini (struct app *app)
{
	for (size_t i = 0; i < 2; ++i) {
		if (app->model[i].request)
			http_cancel(app->http, &app->model[i].request);
		if (app->rapid[i].request)
			http_cancel(app->http, &app->rapid[i].request);
		if (app->probe[i].request)
			http_cancel(app->http, &app->probe[i].request);
		dstr_cleanup(&app->probe[i].response);
		dstr_cleanup(&app->probe[i].rapid_base);
		model_reset(&app->model[i]);
		sse_fini(&app->model[i].sse);
		rapid_reset_turn(&app->rapid[i]);
		dstr_cleanup(&app->generation_tail[i]);
		dstr_cleanup(&app->reasoning_tail[i]);
		dstr_cleanup(&app->transition[i]);
	}

	turn_destroy(&app->pending);

	struct list *node;
	struct list *tmp;
	list_foreach_safe (node, tmp, &app->prompts) {
		struct prompt *prompt = container_of(node, struct prompt, hook);
		list_del(node);
		prompt_destroy(&prompt);
	}
	list_foreach_safe (node, tmp, &app->history) {
		struct turn *turn = container_of(node, struct turn, hook);
		list_del(node);
		turn_destroy(&turn);
	}

	dstr_cleanup(&app->stdin_buf);
	dstr_cleanup(&app->bearer);
	if (app->stdin_ref.fd >= 0)
		loop_unwatch(&app->loop, &app->stdin_ref);
	if (app->stdin_flags >= 0)
		fcntl(STDIN_FILENO, F_SETFL, app->stdin_flags);
	http_destroy(&app->http);
	loop_fini(&app->loop);
}

int
app_run (struct app_cfg const *cfg)
{
	struct app a = app(cfg);
	int e = app_init(&a);
	if (e) {
		app_fini(&a);
		return e;
	}

	e = app_start_template_probes(&a);
	if (!e) {
		app_advance(&a);
		e = loop_exec(&a.loop);
	}
	if (!e)
		e = a.error;
	app_fini(&a);
	return e;
}
