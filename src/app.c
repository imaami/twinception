/** @file Paired-model conversation state and reasoning-trace exchange. */
#include <errno.h>
#include <fcntl.h>
#include <json-c/json.h>
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
	char        *user;
	char        *answer[2];
	char        *reasoning[2];
};

struct prompt {
	struct list  hook;
	char        *text;
};

struct app;

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

struct app {
	struct app_cfg    cfg;
	struct loop_ref   stdin_ref;
	struct list       history;
	struct list       prompts;
	struct model_run  model[2];
	struct turn      *pending;
	struct http      *http;
	struct loop       loop;
	struct buf        stdin_buf;
	int               stdin_flags;
	int               stdin_eof;
	int               exit_after_turn;
};

static void
turn_destroy (struct turn **p_turn)
{
	if (!p_turn || !*p_turn)
		return;
	struct turn *turn = *p_turn;
	*p_turn = nullptr;
	free(turn->reasoning[1]);
	free(turn->reasoning[0]);
	free(turn->answer[1]);
	free(turn->answer[0]);
	free(turn->user);
	free(turn);
}

static void
prompt_destroy (struct prompt **p_prompt)
{
	if (!p_prompt || !*p_prompt)
		return;
	struct prompt *prompt = *p_prompt;
	*p_prompt = nullptr;
	free(prompt->text);
	free(prompt);
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

static char *
request_body (struct app const *app,
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

	if (app->cfg.system &&
	    json_add_message(messages, "system", app->cfg.system, nullptr))
		goto fail;

	struct list *node;
	list_foreach (node, &app->history) {
		struct turn *turn = container_of(node, struct turn, hook);
		if (json_add_message(messages, "user", turn->user, nullptr) ||
		    json_add_message(messages, "assistant",
		                     turn_answer_for(app, turn, model),
		                     turn->reasoning[1 - model]))
			goto fail;
	}

	if (json_add_message(messages, "user", user, nullptr))
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

static void
model_tail_append (struct model_run *model,
                   char const       *data,
                   size_t            len)
{
	enum { MAX_TAIL = 8192 };
	if (len >= MAX_TAIL) {
		buf_reset(&model->tail);
		buf_append(&model->tail, data + len - MAX_TAIL, MAX_TAIL);
		return;
	}

	if (model->tail.len + len > MAX_TAIL)
		buf_consume(&model->tail, model->tail.len + len - MAX_TAIL);
	buf_append(&model->tail, data, len);
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
	model_tail_append(model, data, len);
	int e = sse_feed(&model->sse, data, len);
	if (e) {
		model->error = e;
		return 0;
	}
	return len;
}

static void app_maybe_finish (struct app *app);

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

	switch (model->error) {
	case 0:
		if (result == CURLE_OK) {
			if (status > 199 && status < 300 && model->saw_done)
				break;
			model->error = EPROTO;
		} else {
			model->error = EIO;
		}
		__attribute__((fallthrough));
	default:
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
app_start_prompt (struct app *app,
                  char const *text)
{
	struct turn *turn = calloc(1, sizeof *turn);
	if (!turn)
		return errno ? errno : ENOMEM;
	list_init(&turn->hook);
	turn->user = strdup(text);
	if (!turn->user) {
		turn_destroy(&turn);
		return ENOMEM;
	}

	app->pending = turn;
	for (size_t i = 0; i < ARRAY_SIZE(app->model); ++i) {
		struct model_run *model = &app->model[i];
		model_reset(model);
		char *body = request_body(app, i, text);
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
	if (app->pending)
		return;

	struct prompt *prompt = app_pop_prompt(app);
	if (prompt) {
		int e = app_start_prompt(app, prompt->text);
		prompt_destroy(&prompt);
		if (e) {
			fprintf(stderr, "cannot start request pair: %s\n", strerror(e));
			loop_exit(&app->loop);
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
app_maybe_finish (struct app *app)
{
	if (!app->pending || !app->model[0].done || !app->model[1].done)
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
		}
		list_append(&app->history, &turn->hook);
		app_print_turn(app, turn);
	} else {
		fprintf(stderr, "paired turn discarded; history unchanged\n");
		turn_destroy(&turn);
	}

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
		if (!app->pending)
			loop_exit(&app->loop);
		return 0;
	}

	struct prompt *prompt = calloc(1, sizeof *prompt);
	if (!prompt)
		return ENOMEM;
	list_init(&prompt->hook);
	prompt->text = strndup(text, len);
	if (!prompt->text) {
		prompt_destroy(&prompt);
		return ENOMEM;
	}
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
		sse_init(&app->model[i].sse, model_sse_event, &app->model[i]);
	}

	if (cfg->prompt) {
		app->stdin_eof = 1;
		app->exit_after_turn = 1;
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
		model_reset(&app->model[i]);
		sse_fini(&app->model[i].sse);
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

	app_advance(&app);
	e = loop_exec(&app.loop);
	app_fini(&app);
	return e;
}
