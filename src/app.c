/** @file Paired-model conversation state and reasoning-trace exchange. */
#include <errno.h>
#include <fcntl.h>
#include <json-c/json.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <unistd.h>

#include "app.h"
#include "buf.h"
#include "http.h"
#include "list.h"
#include "loop.h"
#include "sse.h"
#include "util.h"

struct turn {
	struct list  hook;
	char        *user[2];
	char        *answer[2];
	char        *reasoning[2];
};

struct prompt {
	struct list  hook;
	char        *text[2];
};

struct app;

struct template_probe {
	struct http_request *request;
	struct buf           response;
	struct app          *app;
	size_t               index;
	int                  done;
};

struct model_run {
	struct http_request *request;
	struct sse           sse;
	struct buf           answer;
	struct buf           reasoning;
	struct buf           tail;
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
	struct buf           response;
	struct buf           base;
	struct buf           emitted;
	struct buf           chunk;
	struct buf           answer;
	struct buf           tail;
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
	struct buf             stdin_buf;
	char                  *transition[2];
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

static void
turn_destroy (struct turn **p_turn)
{
	if (!p_turn || !*p_turn)
		return;
	struct turn *turn = *p_turn;
	*p_turn = nullptr;
	for (size_t i = 0; i < ARRAY_SIZE(turn->user); ++i) {
		free(turn->reasoning[i]);
		free(turn->answer[i]);
		free(turn->user[i]);
	}
	free(turn);
}

static struct turn *
turn_create (char const *user_a,
             char const *user_b)
{
	struct turn *turn = calloc(1, sizeof *turn);
	if (!turn)
		return nullptr;
	list_init(&turn->hook);

	turn->user[0] = strdup(user_a);
	turn->user[1] = strdup(user_b);
	if (!turn->user[0] || !turn->user[1]) {
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
	free(prompt->text[1]);
	free(prompt->text[0]);
	free(prompt);
}

static struct prompt *
prompt_create (char const *text_a,
               char const *text_b)
{
	struct prompt *prompt = calloc(1, sizeof *prompt);
	if (!prompt)
		return nullptr;
	list_init(&prompt->hook);

	prompt->text[0] = strdup(text_a);
	prompt->text[1] = strdup(text_b);
	if (!prompt->text[0] || !prompt->text[1]) {
		prompt_destroy(&prompt);
		return nullptr;
	}
	return prompt;
}

static int
json_add_message (struct json_object *messages,
                  char const         *role,
                  char const         *content,
                  char const         *reasoning)
{
	struct json_object *msg = json_object_new_object();
	if (!msg)
		return ENOMEM;

	json_object_object_add(msg, "role", json_object_new_string(role));
	json_object_object_add(msg, "content",
	                       json_object_new_string(content ? content : ""));
	if (reasoning)
		json_object_object_add(msg, "reasoning_content",
		                       json_object_new_string(reasoning));

	if (json_object_array_add(messages, msg)) {
		json_object_put(msg);
		return ENOMEM;
	}
	return 0;
}

static void
app_fail (struct app *app,
          int         error)
{
	if (!app->error)
		app->error = error;
	loop_exit(&app->loop);
}

static char *
endpoint_url (char const *chat_url,
              char const *endpoint)
{
	static char const suffix[] = "/v1/chat/completions";
	size_t len = strlen(chat_url);
	size_t suffix_len = sizeof suffix - 1;
	size_t endpoint_len = strlen(endpoint);
	if (len < suffix_len || memcmp(chat_url + len - suffix_len,
	                               suffix, suffix_len))
		return nullptr;

	size_t prefix_len = len - suffix_len;
	char *url = malloc(prefix_len + endpoint_len + 1);
	if (!url)
		return nullptr;
	memcpy(url, chat_url, prefix_len);
	memcpy(url + prefix_len, endpoint, endpoint_len + 1);
	return url;
}

static char const *
turn_answer_for (struct app const  *app,
                 struct turn const *turn,
                 size_t             model)
{
	switch (app->cfg.history_mode) {
	case APP_HISTORY_SHARED_A:
		return turn->answer[0];
	case APP_HISTORY_SHARED_B:
		return turn->answer[1];
	default:
		return turn->answer[model];
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
		if (json_add_message(messages, "user", turn->user[model], nullptr) ||
		    json_add_message(messages, "assistant",
		                     turn_answer_for(app, turn, model),
		                     turn->reasoning[1 - model]))
			return ENOMEM;
	}
	return 0;
}

static char *
template_probe_body (struct app const *app)
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
	if (app->cfg.rapid_quantum) {
		error = json_add_message(messages, "user", "rapid template probe", nullptr) ||
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

	char const *json = json_object_to_json_string_ext(root,
	                                                  JSON_C_TO_STRING_PLAIN);
	char *body = strdup(json);
	json_object_put(root);
	return body;
}

static size_t
template_probe_write (char const *data,
                      size_t      len,
                      void       *user)
{
	struct template_probe *probe = user;
	return buf_append(&probe->response, data, len) ? 0 : len;
}

static int
template_probe_prompt (struct template_probe *probe,
                       char                  **p_prompt)
{
	if (!probe->response.data) {
		fprintf(stderr, "model %c template check failed: empty response\n",
		        (int)('A' + probe->index));
		return EPROTO;
	}

	struct json_object *root = json_tokener_parse(probe->response.data);
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

	*p_prompt = strdup(json_object_get_string(value));
	json_object_put(root);
	return *p_prompt ? 0 : ENOMEM;
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

static int
template_probe_validate_rapid (struct template_probe *probe,
                               char const             *prompt)
{
	char const *reasoning = strstr(prompt, probe_rapid_reasoning);
	char const *answer = reasoning
		? strstr(reasoning + sizeof probe_rapid_reasoning - 1, probe_rapid_answer)
		: nullptr;
	if (!reasoning || !answer) {
		fprintf(stderr,
		        "model %c rapid template check failed: cannot locate reasoning "
		        "and answer prefills in /apply-template output\n",
		        (int)('A' + probe->index));
		return EPROTO;
	}

	char const *begin = reasoning + sizeof probe_rapid_reasoning - 1;
	size_t len = (size_t)(answer - begin);
	if (!len) {
		fprintf(stderr,
		        "model %c rapid template check failed: template has no "
		        "reasoning-to-answer transition\n",
		        (int)('A' + probe->index));
		return EPROTO;
	}

	probe->app->transition[probe->index] = strndup(begin, len);
	if (!probe->app->transition[probe->index])
		return ENOMEM;

	if (probe->app->cfg.debug)
		fprintf(stderr,
		        "model %c rapid template check: captured %zu-byte "
		        "reasoning-to-answer transition\n",
		        (int)('A' + probe->index), len);
	return 0;
}

static int
template_probe_validate (struct template_probe *probe)
{
	char *prompt;
	int error = template_probe_prompt(probe, &prompt);
	if (error)
		return error;

	error = probe->app->cfg.rapid_quantum
		? template_probe_validate_rapid(probe, prompt)
		: template_probe_validate_history(probe, prompt);
	free(prompt);
	return error;
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
	probe->done = 1;

	int error = 0;
	if (result != CURLE_OK || status < 200 || status >= 300) {
		fprintf(stderr, "model %c template check failed: HTTP %ld, curl=%s%s%s\n",
		        (int)('A' + probe->index), status, curl_easy_strerror(result),
		        curl_error && *curl_error ? ": " : "",
		        curl_error && *curl_error ? curl_error : "");
		error = EIO;
	} else {
		error = template_probe_validate(probe);
	}

	if (error) {
		app_fail(app, error);
		return;
	}
	if (!app->probe[0].done || !app->probe[1].done)
		return;

	app->template_ready = 1;
	app_advance(app);
}

static int
app_start_template_probes (struct app *app)
{
	if (!app->cfg.template_check) {
		if (app->cfg.rapid_quantum) {
			fputs("rapid mode requires template transition discovery; "
			      "-P cannot be used with --rapid\n", stderr);
			return EINVAL;
		}
		app->template_ready = 1;
		return 0;
	}

	char *body = template_probe_body(app);
	if (!body)
		return ENOMEM;

	int error = 0;
	for (size_t i = 0; i < ARRAY_SIZE(app->probe); ++i) {
		struct template_probe *probe = &app->probe[i];
		probe->app = app;
		probe->index = i;

		char *url = endpoint_url(app->cfg.url[i], "/apply-template");
		if (!url) {
			fprintf(stderr,
			        "cannot derive /apply-template from model %c URL%s\n",
			        (int)('A' + i),
			        app->cfg.rapid_quantum
			        ? "; rapid mode requires llama-server-compatible endpoints"
			        : "; use -P to skip template validation");
			error = EINVAL;
			break;
		}

		error = http_post_json(app->http, url, body,
		                       template_probe_write, template_probe_done, probe,
		                       &probe->request);
		free(url);
		if (error)
			break;
	}

	free(body);
	return error;
}

static char *
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
	json_object_object_add(root, "stream", json_object_new_boolean(1));
	json_object_object_add(root, "reasoning_format",
	                       json_object_new_string("deepseek"));
	if (app->cfg.model[model])
		json_object_object_add(root, "model",
		                       json_object_new_string(app->cfg.model[model]));
	if (app->cfg.temperature >= 0)
		json_object_object_add(root, "temperature",
		                       json_object_new_double(app->cfg.temperature));
	if (app->cfg.max_tokens > 0)
		json_object_object_add(root, "max_tokens",
		                       json_object_new_int64(app->cfg.max_tokens));

	if ((app->cfg.system &&
	     json_add_message(messages, "system", app->cfg.system, nullptr)) ||
	    json_add_history(messages, app, model) ||
	    json_add_message(messages, "user", user, nullptr))
		goto fail;

	char const *json = json_object_to_json_string_ext(root,
	                                                  JSON_C_TO_STRING_PLAIN);
	char *ret = strdup(json);
	json_object_put(root);
	return ret;

fail:
	json_object_put(root);
	return nullptr;
}

static char *
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

	char const *json = json_object_to_json_string_ext(root,
	                                                  JSON_C_TO_STRING_PLAIN);
	char *body = strdup(json);
	json_object_put(root);
	return body;
}

static void
model_tail_append (struct buf *tail,
                   char const *data,
                   size_t      len)
{
	enum { MAX_TAIL = 8192 };
	if (len >= MAX_TAIL) {
		buf_reset(tail);
		buf_append(tail, data + len - MAX_TAIL, MAX_TAIL);
		return;
	}

	if (tail->len + len > MAX_TAIL)
		buf_consume(tail, tail->len + len - MAX_TAIL);
	buf_append(tail, data, len);
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
		e = buf_append(&model->reasoning, str, n);
	} else if (json_object_object_get_ex(delta, "reasoning", &value) &&
	           json_object_get_type(value) == json_type_string) {
		char const *str = json_object_get_string(value);
		size_t n = (size_t)json_object_get_string_len(value);
		e = buf_append(&model->reasoning, str, n);
	}

	if (!e && json_object_object_get_ex(delta, "content", &value) &&
	    json_object_get_type(value) == json_type_string) {
		char const *str = json_object_get_string(value);
		size_t n = (size_t)json_object_get_string_len(value);
		e = buf_append(&model->answer, str, n);
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
		if (model->tail.len)
			fprintf(stderr, "model %c response tail:\n%.*s\n",
			        (int)('A' + model->index),
			        (int)model->tail.len, model->tail.data);
	}

	app_maybe_finish(model->app);
}

static void
model_reset (struct model_run *model)
{
	sse_fini(&model->sse);
	buf_fini(&model->tail);
	buf_fini(&model->reasoning);
	buf_fini(&model->answer);
	model->error = 0;
	model->done = 0;
	model->saw_done = 0;
	sse_init(&model->sse, model_sse_event, model);
}

static int
app_start_chat (struct app *app)
{
	for (size_t i = 0; i < ARRAY_SIZE(app->model); ++i) {
		struct model_run *model = &app->model[i];
		model_reset(model);
		char *body = chat_request_body(app, i, app->pending->user[i]);
		if (!body)
			return ENOMEM;
		int e = http_post_json(app->http, app->cfg.url[i], body,
		                       model_write, model_done, model,
		                       &model->request);
		free(body);
		if (e)
			return e;
	}
	return 0;
}

static void
rapid_reset_request (struct rapid_model *model)
{
	sse_fini(&model->sse);
	buf_reset(&model->chunk);
	buf_reset(&model->tail);
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
	buf_fini(&model->response);
	buf_fini(&model->base);
	buf_fini(&model->emitted);
	buf_fini(&model->chunk);
	buf_fini(&model->answer);
	buf_fini(&model->tail);
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
	return buf_append(&model->response, data, len) ? 0 : len;
}

static int
rapid_template_parse (struct rapid_model *model)
{
	if (!model->response.data)
		return EPROTO;
	struct json_object *root = json_tokener_parse(model->response.data);
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
		error = buf_append(&model->base, str, len);
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

	struct json_object *value;
	int e = 0;
	if (json_object_object_get_ex(root, "content", &value) &&
	    json_object_get_type(value) == json_type_string) {
		char const *str = json_object_get_string(value);
		size_t n = (size_t)json_object_get_string_len(value);
		struct buf *out = model->app->rapid_stage == RAPID_ANSWER
			? &model->answer : &model->chunk;
		e = buf_append(out, str, n);
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
		if (model->tail.len)
			fprintf(stderr, "model %c response tail:\n%.*s\n",
			        (int)('A' + model->index),
			        (int)model->tail.len, model->tail.data);
	}

	rapid_maybe_advance(model->app);
}

static char *
rapid_body (struct app const *app,
            size_t            model,
            char const       *prompt,
            size_t            prompt_len,
            long              n_predict)
{
	struct json_object *root = json_object_new_object();
	if (!root)
		return nullptr;

	json_object_object_add(root, "prompt",
	                       json_object_new_string_len(prompt, (int)prompt_len));
	json_object_object_add(root, "stream", json_object_new_boolean(1));
	json_object_object_add(root, "cache_prompt", json_object_new_boolean(1));
	if (n_predict > 0)
		json_object_object_add(root, "n_predict",
		                       json_object_new_int64(n_predict));
	if (app->cfg.model[model])
		json_object_object_add(root, "model",
		                       json_object_new_string(app->cfg.model[model]));
	if (app->cfg.temperature >= 0)
		json_object_object_add(root, "temperature",
		                       json_object_new_double(app->cfg.temperature));

	char const *json = json_object_to_json_string_ext(root,
	                                                  JSON_C_TO_STRING_PLAIN);
	char *body = strdup(json);
	json_object_put(root);
	return body;
}

static int
rapid_start_raw (struct app *app,
                 struct rapid_model *model,
                 struct buf const   *foreign,
                 char const         *suffix,
                 long                n_predict)
{
	rapid_reset_request(model);
	model->requested = n_predict > 0 ? (uint32_t)n_predict : 0;
	sse_init(&model->sse, rapid_sse_event, model);

	struct buf prompt = {0};
	int e = buf_append(&prompt, model->base.data, model->base.len);
	if (!e && foreign && foreign->len)
		e = buf_append(&prompt, foreign->data, foreign->len);
	if (!e && suffix)
		e = buf_append(&prompt, suffix, strlen(suffix));
	if (e) {
		buf_fini(&prompt);
		return e;
	}

	char *body = rapid_body(app, model->index, prompt.data, prompt.len, n_predict);
	buf_fini(&prompt);
	if (!body)
		return ENOMEM;

	char *url = endpoint_url(app->cfg.url[model->index], "/completion");
	if (!url) {
		free(body);
		return EINVAL;
	}
	e = http_post_json(app->http, url, body, rapid_write, rapid_done, model,
	                   &model->request);
	free(url);
	free(body);
	return e;
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
		int e = rapid_start_raw(app, &app->rapid[i],
		                        &app->rapid[1 - i].emitted, nullptr, n);
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
		int e = rapid_start_raw(app, &app->rapid[i],
		                        &app->rapid[1 - i].emitted,
		                        app->transition[i], app->cfg.max_tokens);
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
	        app->rapid_round, app->rapid[0].tokens,
	        app->rapid[0].chunk.data ? app->rapid[0].chunk.data : "",
	        app->rapid_round, app->rapid[1].tokens,
	        app->rapid[1].chunk.data ? app->rapid[1].chunk.data : "");
	fflush(stdout);
}

static void
rapid_commit (struct app *app)
{
	struct turn *turn = app->pending;
	app->pending = nullptr;

	for (size_t i = 0; i < 2; ++i) {
		turn->answer[i] = buf_take(&app->rapid[i].answer);
		turn->reasoning[i] = buf_take(&app->rapid[i].emitted);
		if (!turn->answer[i])
			turn->answer[i] = strdup("");
		if (!turn->reasoning[i])
			turn->reasoning[i] = strdup("");
		if (!turn->answer[i] || !turn->reasoning[i]) {
			turn_destroy(&turn);
			app_fail(app, ENOMEM);
			return;
		}
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
		for (size_t i = 0; i < 2; ++i)
			e = e ? e : buf_append(&app->rapid[i].emitted,
			                       app->rapid[i].chunk.data,
			                       app->rapid[i].chunk.len);
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

		char *body = template_turn_body(app, i, app->pending->user[i]);
		if (!body)
			return ENOMEM;
		char *url = endpoint_url(app->cfg.url[i], "/apply-template");
		if (!url) {
			free(body);
			return EINVAL;
		}
		int e = http_post_json(app->http, url, body,
		                       rapid_template_write, rapid_template_done, model,
		                       &model->request);
		free(url);
		free(body);
		if (e)
			return e;
	}
	return 0;
}

static int
app_start_prompt (struct app *app,
                  char const *user_a,
                  char const *user_b,
                  int         human)
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
	struct q {
		char const *a;
		char const *b;
		char const *c;
	};

	if (app->cfg.debug) {
		struct q r0 = turn->reasoning[0]
			? (typeof(r0)){"\033[1;35m", turn->reasoning[0], "\033[0m"}
			: (typeof(r0)){"", "", ""};
		struct q r1 = turn->reasoning[1]
			? (typeof(r1)){"\033[1;36m", turn->reasoning[1], "\033[0m"}
			: (typeof(r1)){"", "", ""};
		printf("\n--- A reasoning -> B ---\n%s%s%s\n"
		       "\n--- B reasoning -> A ---\n%s%s%s\n",
		       r0.a, r0.b, r0.c, r1.a, r1.b, r1.c);
	}

	struct q a0 = turn->answer[0]
		? (typeof(a0)){"\033[1;35m", turn->answer[0], "\033[0m"}
		: (typeof(a0)){"", "", ""};
	struct q a1 = turn->answer[1]
		? (typeof(a1)){"\033[1;36m", turn->answer[1], "\033[0m"}
		: (typeof(a1)){"", "", ""};
	printf("\n[A]\n%s%s%s\n\n[B]\n%s%s%s\n",
	       a0.a, a0.b, a0.c, a1.a, a1.b, a1.c);
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
	int e = app_start_prompt(app, turn->answer[1], turn->answer[0], 0);
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
		int e = app_start_prompt(app, prompt->text[0], prompt->text[1], 1);
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
			turn->answer[i] = buf_take(&app->model[i].answer);
			turn->reasoning[i] = buf_take(&app->model[i].reasoning);
			if (!turn->answer[i])
				turn->answer[i] = strdup("");
			if (!turn->reasoning[i])
				turn->reasoning[i] = strdup("");
			if (!turn->answer[i] || !turn->reasoning[i]) {
				turn_destroy(&turn);
				app_fail(app, ENOMEM);
				return;
			}
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

	char *str = strndup(text, len);
	if (!str)
		return ENOMEM;
	struct prompt *prompt = prompt_create(str, str);
	free(str);
	if (!prompt)
		return ENOMEM;
	list_append(&app->prompts, &prompt->hook);
	return 0;
}

static int
app_stdin_event (struct loop     *loop,
                 struct loop_ref *ref,
                 uint32_t         events)
{
	(void)loop;
	struct app *app = ref->owner;
	char data[4096];

	if (events & (EPOLLHUP | EPOLLRDHUP))
		app->stdin_eof = 1;

	for (;;) {
		ssize_t n = read(STDIN_FILENO, data, sizeof data);
		if (n > 0) {
			int e = buf_append(&app->stdin_buf, data, (size_t)n);
			if (e)
				return e;
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

	size_t used = 0;
	while (used < app->stdin_buf.len) {
		char *line = app->stdin_buf.data + used;
		char *nl = memchr(line, '\n', app->stdin_buf.len - used);
		if (!nl)
			break;
		size_t len = (size_t)(nl - line);
		int e = app_queue_prompt(app, line, len);
		if (e)
			return e;
		used += len + 1;
	}
	if (used)
		buf_consume(&app->stdin_buf, used);

	if (app->stdin_eof && app->stdin_buf.len) {
		int e = app_queue_prompt(app, app->stdin_buf.data, app->stdin_buf.len);
		if (e)
			return e;
		buf_reset(&app->stdin_buf);
	}

	app_advance(app);
	return 0;
}

static int
app_init (struct app           *app,
          struct app_cfg const *cfg)
{
	*app = (struct app){
		.cfg = *cfg,
		.loop.epfd = -1,
		.stdin_flags = -1
	};
	app->stdin_ref.fd = -1;
	list_init(&app->loop.refs);
	list_init(&app->history);
	list_init(&app->prompts);

	int e = loop_init(&app->loop);
	if (e)
		return e;

	e = http_init(&app->http, &app->loop);
	if (e)
		return e;

	for (size_t i = 0; i < 2; ++i) {
		app->model[i].app = app;
		app->model[i].index = i;
		app->probe[i].app = app;
		app->probe[i].index = i;
		app->rapid[i].app = app;
		app->rapid[i].index = i;
		sse_init(&app->model[i].sse, model_sse_event, &app->model[i]);
	}

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
		buf_fini(&app->probe[i].response);
		model_reset(&app->model[i]);
		sse_fini(&app->model[i].sse);
		rapid_reset_turn(&app->rapid[i]);
		free(app->transition[i]);
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

	buf_fini(&app->stdin_buf);
	if (app->stdin_ref.fd >= 0)
		loop_unwatch(&app->loop, &app->stdin_ref);
	if (app->stdin_flags >= 0)
		fcntl(STDIN_FILENO, F_SETFL, app->stdin_flags);
	http_fini(&app->http);
	loop_fini(&app->loop);
}

int
app_run (struct app_cfg const *cfg)
{
	struct app app;
	int e = app_init(&app, cfg);
	if (e) {
		app_fini(&app);
		return e;
	}

	e = app_start_template_probes(&app);
	if (!e) {
		app_advance(&app);
		e = loop_exec(&app.loop);
	}
	if (!e)
		e = app.error;
	app_fini(&app);
	return e;
}
