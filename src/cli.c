/** @file Unix-socket MCP chat client. Inference, tools and notes belong to AskAI. */
#define _GNU_SOURCE

#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <locale.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "autocomplete_file.h"
#include "buf.h"
#include "cli_client.h"
#include "cli_console.h"
#include "cli_io.h"
#include "cli_loop.h"
#include "cli_mailbox.h"
#include "cli_network.h"
#include "cli_signals.h"
#include "cli_state.h"
#include "cli_ui.h"
#include "compat.h"
#include "dstr.h"
#include "jb.h"

static struct cstr_view
cli_trim (struct cstr_view text)
{
	while (text.len && strchr("\r\n \t", text.data[text.len - 1]) && text.data[text.len - 1])
		--text.len;
	while (text.len && (*text.data == ' ' || *text.data == '\t'))
		text = cstr_view_slice(text, 1, SIZE_MAX);
	return text;
}

static int
cli_read_file (struct dstr      *text,
               struct cstr_view  path,
               struct cli_error *error)
{
	dstr_auto expanded = dstr(nullptr), contents = dstr(nullptr);
	__attribute__((cleanup(buf_fini))) struct buf bytes = {};
	if (!path.len || !path.data || memchr(path.data, 0, path.len))
		return EINVAL;
	char const *home = getenv("HOME");
	if (home && (cstr_view_equal(path, CSTR_VIEW("~")) || cstr_view_starts(path, CSTR_VIEW("~/")))) {
		if (!dstr_cat(&expanded, home) || !dstr_cat_n(&expanded, path.data + 1, path.len - 1))
			return ENOMEM;
	} else if (!dstr_cat_n(&expanded, path.data, path.len)) {
		return ENOMEM;
	}
	FILE *file = fopen(dstr_get(&expanded), "rb");
	if (!file)
		return cli_error_set(error, errno, "%s: %s", dstr_get(&expanded), strerror(errno));
	int e = 0;
	char chunk[8192];
	size_t count;
	/* A file read can end inside a UTF-8 character. Assemble bytes first. */
	while ((count = fread(chunk, 1, sizeof chunk, file))) {
		if (count > CLI_LIMIT - bytes.tail) {
			e = cli_error_set(error, EFBIG, "Prompt file exceeds the 16 MiB client limit");
			break;
		}
		struct span room;
		e = buf_reserve(&bytes, count, CLI_LIMIT, &room);
		if (e)
			break;
		memcpy(room.ptr, chunk, count);
		buf_commit(&bytes, count);
		if (count < sizeof chunk)
			break;
	}
	if (!e && ferror(file))
		e = cli_error_set(error, EIO, "%s: read failed", dstr_get(&expanded));
	if (fclose(file) && !e)
		e = errno;
	file = nullptr;
	struct cstr_view result = {(char const *)bytes.data, bytes.tail};
	if (!e && result.len && (memchr(result.data, 0, result.len) || jb_utf8(result.data, result.len) != result.len))
		e = cli_error_set(error, EILSEQ, "Prompt files must contain UTF-8 text without NUL bytes");
	if (!e && !dstr_cat_n(&contents, result.data, result.len))
		e = ENOMEM;
	if (!e) {
		dstr_cleanup(text);
		*text = dstr_move(&contents);
	}
	return e;
}

struct cli_options {
	char const  *socket;
	char const  *file;
	char const  *tool;
	char const  *system;
	char const  *history_repo;
	char const  *session_id;
	char const  *color;
	struct dstr  prompt;
	bool         has_prompt;
	bool         has_system;
	bool         single;
	bool         simple;
	bool         multiline;
	bool         reasoning;
	bool         no_history;
	bool         done;
};

static int
cli_help (void)
{
	return puts("Usage: askai-cli --socket PATH [options]\n"
	            "Chat with AskAI's upstream LLM over its Unix socket.\n\n"
	            "  --socket PATH            AskAI socket (or ASKAI_SOCKET)\n"
	            "  --tool NAME              Select the configured ask tool\n"
	            "  -p, --prompt TEXT        Send an initial prompt\n"
	            "  -f, --file FILE          Read the initial prompt from a text file\n"
	            "  --system TEXT            Override server system instructions\n"
	            "  --single-turn            Exit after one answer\n"
	            "  --simple-io              Basic line input; automatic for pipes\n"
	            "  --multiline-input        Enter adds a line; finish with a trailing /\n"
	            "  --color auto|on|off       Terminal colors (default auto)\n"
	            "  --no-reasoning            Hide exposed reasoning\n"
	            "  --history-repo PATH      Separate Git repository for client history\n"
	            "  --session-id ID          Override the saved-history session identity\n"
	            "  --no-history             Disable saved client history\n"
	            "  -h, --help               Show this help\n\n"
	            "Commands: /help, /clear, /regen, /read FILE, /exit\n"
	            "          /tools [NAME], /tool NAME key=VALUE ..., /names, /refresh\n"
	            "Tab completes exposed tools, parameters, values and participant names.\n"
	            "PgUp/PgDn scroll; Ctrl-End follows output; Ctrl-R searches input history.\n"
	            "End a line with \\ to continue it. Ctrl-C cancels the current answer;\n"
	            "while entering a prompt it clears with undo. Ctrl-D exits. Tools run in AskAI.") < 0 ? EIO : 0;
}

static int
cli_args (struct cli_options  *out,
          int                  argc,
          char               **argv,
          struct cli_error    *error)
{
	out->socket = getenv("ASKAI_SOCKET");
	static struct option const options[] = {
		{"socket", required_argument, nullptr, 's'},
		{"tool", required_argument, nullptr, 't'},
		{"prompt", required_argument, nullptr, 'p'},
		{"file", required_argument, nullptr, 'f'},
		{"system", required_argument, nullptr, 'S'},
		{"single-turn", no_argument, nullptr, '1'},
		{"simple-io", no_argument, nullptr, 'i'},
		{"multiline-input", no_argument, nullptr, 'm'},
		{"color", required_argument, nullptr, 'c'},
		{"no-reasoning", no_argument, nullptr, 'r'},
		{"history-repo", required_argument, nullptr, 'H'},
		{"session-id", required_argument, nullptr, 'I'},
		{"no-history", no_argument, nullptr, 'N'},
		{"help", no_argument, nullptr, 'h'},
		{"version", no_argument, nullptr, 'v'},
		{}
	};
	int c;
	while ((c = getopt_long(argc, argv, "hp:f:", options, nullptr)) != -1) {
		switch (c) {
		case 's': out->socket = optarg; break;
		case 't': out->tool = optarg; break;
		case 'p': case 'f': {
			if (out->has_prompt)
				return cli_error_set(error, EINVAL, "Use only one --prompt or --file");
			int e = c == 'p' && !dstr_cat(&out->prompt, optarg) ? ENOMEM : 0;
			if (c == 'f')
				out->file = optarg;
			if (e)
				return e;
			out->has_prompt = true;
			break;
		}
		case 'S': out->system = optarg; out->has_system = true; break;
		case '1': out->single = true; break;
		case 'i': out->simple = true; break;
		case 'm': out->multiline = true; break;
		case 'c': out->color = optarg; break;
		case 'r': out->reasoning = false; break;
		case 'H': out->history_repo = optarg; break;
		case 'I':
			out->session_id = optarg;
			if (!*optarg)
				return cli_error_set(error, EINVAL, "--session-id requires a nonempty identity");
			break;
		case 'N': out->no_history = true; break;
		case 'h': out->done = true; return cli_help();
		case 'v': out->done = true; return puts("askai-cli 1") < 0 ? EIO : 0;
		default: return cli_error_set(error, EINVAL, "Invalid option; see --help");
		}
	}
	if (optind != argc)
		return cli_error_set(error, EINVAL, "Unexpected positional argument; see --help");
	if (out->socket && !strncmp(out->socket, "unix:", 5))
		out->socket += 5;
	if (!out->socket || !*out->socket)
		return cli_error_set(error, EINVAL, "Specify --socket PATH or ASKAI_SOCKET");
	if (out->no_history && out->history_repo && *out->history_repo)
		return cli_error_set(error, EINVAL, "--history-repo and --no-history are mutually exclusive");
	if (strcmp(out->color, "auto") && strcmp(out->color, "on") && strcmp(out->color, "off"))
		return cli_error_set(error, EINVAL, "--color must be auto, on or off");
	return 0;
}

struct cli {
	struct cli_loop           reactor;
	struct cli_mailbox        mailbox;
	struct cli_client         client;
	struct cli_signals        signals;
	struct cli_network       *network;
	struct cli_io            *io;
	struct cli_console       *console;
	struct cli_state         *saved;
	struct autocomplete_file *files;
	struct json_object       *history;
	struct cli_options const *options;
	uint64_t                  exit_due;
	unsigned                  exit_signal;
	int                       ui_error;
	bool                      busy;
	bool                      tty;
	_Atomic bool              cancel_io;
};

static void
cli_signal (struct cli_signals *signals,
            unsigned            number)
{
	struct cli *cli = container_of(signals, struct cli, signals);
	int e = 0;
	switch (number) {
	case SIGWINCH:
		if (cli->console)
			e = cli_console_resize(cli->console);
		break;
	case SIGTSTP:
		if (cli->console)
			e = cli_console_suspend(cli->console);
		if (!e && kill(getpid(), SIGSTOP))
			e = errno;
		break;
	case SIGCONT:
		if (cli->console)
			e = cli_console_resume(cli->console);
		break;
	case SIGINT:
		if (!cli->options->single && cli->tty &&
		    (!cli->options->simple || cli->busy || cli_network_active(cli->network))) {
			if (cli->busy || cli_network_active(cli->network)) {
				atomic_store(&cli->cancel_io, true);
				cli_network_cancel(cli->network, false);
			} else if (cli->console) {
				e = cli_console_interrupt(cli->console, false);
			}
			break;
		}
		[[fallthrough]];
	case SIGTERM:
	case SIGHUP:
		if (!cli->exit_signal || cli->exit_signal == SIGINT)
			cli->exit_signal = number;
		if (!cli->exit_due)
			cli->exit_due = cli_loop_clock() + 2'000'000'000;
		cli->reactor.until = cli->exit_due;
		atomic_store(&cli->cancel_io, true);
		cli_network_cancel(cli->network, true);
		if (cli->console)
			e = cli_console_interrupt(cli->console, true);
		break;
	}
	if (e && !cli->ui_error)
		cli->ui_error = e;
}

/** A wait may leave a stuck syscall to process exit, but must never unwind
 * storage that its worker still owns. Restore the terminal without freeing
 * any of those objects, then let the kernel reclaim the entire process. */
static void
cli_force_exit (struct cli *cli)
{
	if (cli->console)
		(void)cli_console_suspend(cli->console);
	_Exit(cli->exit_signal ? 128 + (int)cli->exit_signal : 1);
}

static void
cli_service (struct cli_loop *loop)
{
	struct cli *cli = container_of(loop, struct cli, reactor);
	if (cli->exit_due && cli_loop_clock() >= cli->exit_due)
		cli_force_exit(cli);
	cli_network_service(cli->network);
	if (cli->console) {
		int e = cli_console_service(cli->console);
		if (e && e != EINTR && !cli->ui_error)
			cli->ui_error = e;
	}
	if (cli->ui_error) {
		atomic_store(&cli->cancel_io, true);
		cli_network_cancel(cli->network, true);
		if (!cli->exit_due)
			cli->exit_due = cli_loop_clock() + 2'000'000'000;
		cli->reactor.until = cli->exit_due;
	}
}

static bool
cli_output_ready (void *data)
{
	struct cli *cli = data;
	return !cli->ui_error && !cli_console_blocked(cli->console);
}

static int
cli_report (struct cli_client *client,
            struct cstr_view   title,
            struct cstr_view   detail)
{
	struct cli *cli = container_of(client, struct cli, client);
	int e = cli_ui_error(cli->console, title, detail);
	if (e && !cli->ui_error)
		cli->ui_error = e;
	return e;
}

struct cli_work {
	struct cli_io_job job;
	int            (*fn) (void *data);
	void            *data;
	uint64_t         done;
};

static int
cli_work_run (struct cli_io_job *job)
{
	struct cli_work *work = container_of(job, struct cli_work, job);
	return work->fn(work->data);
}

static void
cli_work_done (struct cli_io_job *job)
{
	struct cli_work *work = container_of(job, struct cli_work, job);
	work->done = true;
}

static int
cli_work_wait (struct cli *cli,
               int       (*fn) (void *data),
               void       *data)
{
	if (cli->exit_signal)
		return EINTR;
	struct cli_work work = {.job = {.work = cli_work_run, .done = cli_work_done},
	                        .fn = fn, .data = data};
	int e = cli_io_submit(cli->io, &work.job);
	if (e)
		return e;
	bool busy = cli->busy;
	cli->busy = true;
	while (!work.done) {
		uint64_t due = cli->exit_due ? cli->exit_due : UINT64_MAX;
		int turn = cli_loop_turn(&cli->reactor, due);
		if (turn)
			cli_force_exit(cli);
		if (cli->exit_due && cli_loop_clock() >= cli->exit_due && !work.done)
			cli_force_exit(cli);
	}
	cli->busy = busy;
	if (!busy && !cli->exit_signal)
		atomic_store(&cli->cancel_io, false);
	return cli->exit_signal ? EINTR : work.job.error;
}

enum cli_history_operation { CLI_HISTORY_OPEN, CLI_HISTORY_INPUT, CLI_HISTORY_TURN, CLI_HISTORY_CLEAR };

struct cli_history_work {
	struct cli_state           *state;
	struct cstr_view             path;
	struct cstr_view             text;
	struct cstr_view             answer;
	_Atomic bool const         *cancel;
	enum cli_history_operation  operation;
	int                         replace;
};

static int
cli_history_work (void *data)
{
	struct cli_history_work *work = data;
	switch (work->operation) {
	case CLI_HISTORY_OPEN: {
		int e = 0;
		work->state = cli_state_create(work->path, work->text, work->cancel, &e);
		return e;
	}
	case CLI_HISTORY_INPUT:
		return cli_state_add_input(work->state, work->text);
	case CLI_HISTORY_TURN:
		return cli_state_add_turn(work->state, work->text, work->answer, work->replace);
	case CLI_HISTORY_CLEAR:
		return cli_state_clear_turns(work->state);
	}
	return EINVAL;
}

static int
cli_history_update (struct cli                  *cli,
                    enum cli_history_operation  operation,
                    struct cstr_view            text,
                    struct cstr_view            answer,
                    bool                        replace)
{
	struct cli_history_work work = {.state = cli->saved, .operation = operation,
	                                .text = text, .answer = answer, .replace = replace};
	return cli_work_wait(cli, cli_history_work, &work);
}

struct cli_file_work {
	struct dstr      *text;
	struct cstr_view  path;
	struct cli_error *error;
};

static int
cli_file_work (void *data)
{
	struct cli_file_work *work = data;
	return cli_read_file(work->text, work->path, work->error);
}

static int
cli_file_read (struct cli       *cli,
               struct dstr      *text,
               struct cstr_view  path,
               struct cli_error *error)
{
	struct cli_file_work work = {.text = text, .path = path, .error = error};
	return cli_work_wait(cli, cli_file_work, &work);
}

static int
cli_save_error (struct cli       *cli,
                int               e,
                struct cli_error *error)
{
	if (!e)
		return 0;
	struct cstr_view recovery = cli_state_recovery_file(cli->saved);
	return cli_error_set(error, e, "Cannot save client history: %s%s%.*s", strerror(e),
	                     recovery.len ? ". Recovery snapshot in history repository: " : "",
	                     (int)recovery.len, recovery.data ? recovery.data : "");
}

struct cli_completion_work {
	struct autocomplete_file         *files;
	struct cstr_view                  line;
	struct autocomplete_match const *matches;
	size_t                           cursor;
	size_t                           count;
};

static int
cli_completion_work (void *data)
{
	struct cli_completion_work *work = data;
	struct cstr_view const commands[] = {CSTR_VIEW("/read ")};
	return autocomplete_file_complete(work->files, work->line, work->cursor,
	                                  commands, array_size(commands), &work->matches, &work->count);
}

static int
cli_complete (void                             *context,
              struct cstr_view                  line,
              size_t                            cursor,
              struct autocomplete_match const **matches,
              size_t                           *count)
{
	struct cli *cli = context;
	int e = autocomplete_complete(cli->client.catalog, line, cursor, matches, count);
	if (!e && !*count) {
		struct cli_completion_work work = {.files = cli->files, .line = line, .cursor = cursor};
		e = cli_work_wait(cli, cli_completion_work, &work);
		*matches = work.matches;
		*count = work.count;
	}
	if (e) {
		*matches = nullptr;
		*count = 0;
	}
	return e == ENOMEM ? e : 0;
}

struct cli_path_work {
	char const  *path;
	struct dstr *identity;
};

static int
cli_path_work (void *data)
{
	struct cli_path_work *work = data;
	char *path = realpath(work->path, nullptr);
	if (!path)
		return errno;
	int e = dstr_cat(work->identity, "socket=") && dstr_cat(work->identity, path) ? 0 : ENOMEM;
	free(path);
	path = nullptr;
	return e;
}

static int
cli_history_open (struct cli       *cli,
                  struct cli_error *error)
{
	struct cli_options const *options = cli->options;
	if (options->no_history || (!cli->tty && (!options->history_repo || !*options->history_repo)))
		return 0;
	dstr_auto identity = dstr(nullptr);
	if (options->session_id || dstr_len(&cli->client.session_key)) {
		char const *key = options->session_id ? options->session_id : dstr_get(&cli->client.session_key);
		if (!dstr_cat(&identity, "session=") || !dstr_cat(&identity, key))
			return ENOMEM;
	} else {
		struct cli_path_work work = {.path = options->socket, .identity = &identity};
		int e = cli_work_wait(cli, cli_path_work, &work);
		if (e)
			return cli_error_set(error, e, "Cannot identify history connection: %s", strerror(e));
	}
	struct cstr_view parts[] = {
		CSTR_VIEW("\0participant="), cstr_view_dstr(&cli->client.participant),
		CSTR_VIEW("\0tool="), cstr_view_dstr(&cli->client.tool),
		CSTR_VIEW("\0system-policy="), options->has_system ? CSTR_VIEW("override") : CSTR_VIEW("default"),
		CSTR_VIEW("\0system="), cstr_view(options->system)
	};
	for (size_t i = 0; i < sizeof parts / sizeof *parts; ++i)
		if (!dstr_cat_n(&identity, parts[i].data, parts[i].len))
			return ENOMEM;
	int e = 0;
	struct cli_history_work work = {.operation = CLI_HISTORY_OPEN, .cancel = &cli->cancel_io,
	                                .path = cstr_view(options->history_repo), .text = cstr_view_dstr(&identity)};
	e = cli_work_wait(cli, cli_history_work, &work);
	cli->saved = work.state;
	if (e)
		return cli_error_set(error, e, "Cannot open client history repository: %s", strerror(e));
	for (size_t i = 0, n = cli_state_input_count(cli->saved); !e && i < n; ++i) {
		struct cstr_view input = cli_state_input_at(cli->saved, i);
		e = cli_console_remember(cli->console, input);
	}
	for (size_t i = 0, n = cli_state_turn_count(cli->saved); !e && i < n; ++i) {
		struct cli_state_turn turn = cli_state_turn_at(cli->saved, i);
		e = jb_push(cli->history, jb_object("prompt", cli_client_string(turn.prompt),
		                                   "answer", cli_client_string(turn.answer), nullptr));
	}
	if (!e && cli->tty && json_object_array_length(cli->history)) {
		dstr_auto message = dstr(nullptr);
		if (!dstr_printf(&message, "Restored %zu completed turns. /history displays them; /clear starts over.",
		                 json_object_array_length(cli->history)))
			return ENOMEM;
		e = cli_ui_info(cli->console, cstr_view_dstr(&message));
	}
	return e;
}

struct cli_stream {
	struct cli_ui    ui;
	struct dstr      text;
	struct cstr_view actor;
	struct cstr_view token;
	int64_t          turn;
	bool             reasoning;
};

static int
cli_progress (struct cli_stream  *stream,
              struct json_object *event)
{
	if (!cli_client_is(jb_get(event, "method"), "notifications/progress"))
		return 0;
	struct json_object *params = jb_get(event, "params");
	struct cstr_view token = cli_client_text(jb_get(params, "progressToken"));
	struct cstr_view message = cli_client_text(jb_get(params, "message"));
	if (!token.data || !message.data || !cstr_view_equal(token, stream->token))
		return 0;
	struct json_object *meta = jb_get(jb_get(params, "_meta"), "askai");
	return cli_client_is(jb_get(meta, "kind"), "tool") ? cli_ui_action(&stream->ui, stream->actor, message)
	                                                   : cli_ui_progress(&stream->ui, stream->actor, message);
}

static int
cli_notify (void               *context,
            struct json_object *event,
            int64_t             id,
            struct cli_error   *error)
{
	struct cli_stream *stream = context;
	int e = cli_progress(stream, event);
	if (e || !cli_client_is(jb_get(event, "method"), "notifications/askai/delta"))
		return e;
	struct json_object *params = jb_get(event, "params");
	struct json_object *request = jb_get(params, "requestId");
	if (!json_object_is_type(request, json_type_int) || json_object_get_int64(request) != id)
		return cli_error_set(error, EPROTO, "Unexpected streaming request id");
	struct cstr_view text = cli_client_text(jb_get(params, "text"));
	struct cstr_view kind = cli_client_text(jb_get(params, "kind"));
	if (!text.data || !kind.data)
		return cli_error_set(error, EPROTO, "Expected a JSON string from AskAI");
	int64_t current = json_object_get_int64(jb_get(params, "turn"));
	if (current != stream->turn) {
		if (dstr_len(&stream->text))
			e = cli_ui_push(&stream->ui, false, CSTR_VIEW("\n"));
		dstr_clear(&stream->text);
		stream->turn = current;
	}
	if (e)
		return e;
	if (cstr_view_equal(kind, CSTR_VIEW("text"))) {
		if (text.len > CLI_LIMIT - dstr_len(&stream->text))
			return cli_error_set(error, EFBIG, "Assistant turn exceeds the client limit");
		if (!dstr_cat_n(&stream->text, text.data, text.len))
			return ENOMEM;
		return cli_ui_push(&stream->ui, false, text);
	}
	if (cstr_view_equal(kind, CSTR_VIEW("reasoning")))
		return stream->reasoning ? cli_ui_push(&stream->ui, true, text) : 0;
	return cli_error_set(error, EPROTO, "Unknown AskAI delta kind");
}

static int
cli_ask (struct cli          *cli,
         struct cstr_view     prompt,
         size_t               turns,
         struct json_object **answer,
         struct cli_error    *error)
{
	*answer = nullptr;
	cli->client.notes_dirty |= dstr_len(&cli->client.notes_tool) != 0;
	if (turns >= 512)
		return cli_error_set(error, E2BIG, "Conversation has 512 turns; use /clear");
	if (prompt.len > CLI_LIMIT)
		return cli_error_set(error, EFBIG, "Conversation exceeds the client limit; use /clear");
	size_t bytes = prompt.len;
	cli_json_auto messages = json_object_new_array();
	if (!messages)
		return ENOMEM;
	char const *keys[] = {"prompt", "answer"};
	char const *roles[] = {"user", "assistant"};
	for (size_t i = 0; i < turns; ++i) {
		struct json_object *turn = jb_at(cli->history, i);
		for (size_t j = 0; j < 2; ++j) {
			struct json_object *value = jb_get(turn, keys[j]);
			struct cstr_view text = cli_client_text(value);
			if (text.len > CLI_LIMIT - bytes)
				return cli_error_set(error, EFBIG, "Conversation exceeds the client limit; use /clear");
			bytes += text.len;
			int e = jb_push(messages, jb_object("role", json_object_new_string(roles[j]),
			                                    "content", json_object_get(value), nullptr));
			if (e)
				return e;
		}
	}
	cli_json_auto args = jb_object("prompt", cli_client_string(prompt),
	                               "background", json_object_new_boolean(false),
	                               "messages", json_object_get(messages), nullptr);
	if (!args)
		return ENOMEM;
	if (cli->options->has_system && jb_set(args, "system", cli_client_string(cstr_view(cli->options->system))))
		return ENOMEM;
	struct json_object *params = jb_object("name", cli_client_string(cstr_view_dstr(&cli->client.tool)),
	                                      "arguments", json_object_get(args),
	                                      "_meta", jb_object("askai", jb_object("stream", json_object_new_boolean(true), nullptr),
	                                                         "progressToken", json_object_new_string("cli"), nullptr), nullptr);
	struct cli_stream stream = {.ui = cli_ui(cli->console), .text = DSTR_INIT,
	                            .actor = cstr_view_dstr(&cli->client.tool), .token = CSTR_VIEW("cli"),
	                            .reasoning = cli->options->reasoning};
	cli_json_auto result = nullptr;
	int e = cli_client_rpc(&cli->client, "tools/call", params, cli_notify, &stream, &result, error);
	struct json_object *content = jb_get(result, "content");
	struct json_object *first = jb_at(content, 0);
	struct json_object *value = jb_get(first, "text");
	struct cstr_view text = cli_client_text(value);
	if (!e && !first)
		e = cli_error_set(error, EPROTO, "AskAI returned no assistant text");
	if (!e && (!cli_client_is(jb_get(first, "type"), "text") || !text.data))
		e = cli_error_set(error, EPROTO, "AskAI returned unsupported assistant content");
	if (!e && json_object_get_boolean(jb_get(result, "isError")))
		e = cli_error_set(error, EIO, "%.*s", (int)text.len, text.data);
	if (!e) {
		struct cstr_view streamed = cstr_view_dstr(&stream.text);
		if (cstr_view_starts(text, streamed))
			e = cli_ui_push(&stream.ui, false, cstr_view_slice(text, streamed.len, SIZE_MAX));
		else if (streamed.len && text.len >= streamed.len &&
		         cstr_view_equal(cstr_view_slice(text, text.len - streamed.len, SIZE_MAX), streamed)) {
			/* Agenda wakeups may include already displayed earlier turns. */
		} else {
			if (streamed.len)
				e = cli_ui_push(&stream.ui, false, CSTR_VIEW("\n"));
			if (!e)
				e = cli_ui_push(&stream.ui, false, text);
		}
	}
	int finish = cli_ui_finish(&stream.ui);
	if (!e)
		e = finish;
	if (!e)
		*answer = json_object_get(value);
	cli_ui_fini(&stream.ui);
	dstr_cleanup(&stream.text);
	return e;
}

static int
cli_tools (struct cli       *cli,
           struct cstr_view  name,
           struct cli_error *error)
{
	if (name.len) {
		struct autocomplete_tool const *entry = autocomplete_find_tool(cli->client.catalog,
		                                                               name);
		if (!entry)
			return cli_error_set(error, ENOENT, "No accessible tool named %.*s", (int)name.len, name.data);
		int e = cli_ui_info(cli->console, entry->name);
		if (!e && entry->description.len)
			e = cli_ui_message(cli->console, entry->description);
		struct cstr_view wire;
		if (!e)
			e = cli_client_wire(entry->schema, JSON_C_TO_STRING_PRETTY, &wire, error);
		return e ? e : cli_ui_message(cli->console, wire);
	}

	int ret = 0;
	struct dstr line = DSTR_INIT;

	for (size_t i = 0, count = autocomplete_tool_count(cli->client.catalog); i < count; ++i) {
		struct autocomplete_tool const *entry = autocomplete_tool_at(cli->client.catalog, i);
		struct cstr_view summary = entry->description;
		summary = cstr_view_slice(summary, 0, 160);
		char const *newline = summary.len ? memchr(summary.data, '\n', summary.len) : nullptr;
		if (newline)
			summary.len = (size_t)(newline - summary.data);

		dstr_clear(&line);
		if (!dstr_cat_n(&line, entry->name.data, entry->name.len) ||
		    (summary.len && (!dstr_cat(&line, "  ") || !dstr_cat_n(&line, summary.data, summary.len)))) {
			ret = ENOMEM;
			break;
		}

		int e = cli_ui_message(cli->console, cstr_view_dstr(&line));
		if (e) {
			ret = e;
			break;
		}
	}

	dstr_cleanup(&line);
	return ret;
}

static int
cli_tool_notify (void               *context,
                 struct json_object *event,
                 int64_t             id,
                 struct cli_error   *error)
{
	(void)id;
	(void)error;
	return cli_progress(context, event);
}

static int
cli_tool_content (struct cli         *cli,
                  struct json_object *result,
                  struct cli_error   *error)
{
	struct json_object *content = jb_get(result, "content");
	if (json_object_is_type(content, json_type_array)) {
		for (size_t i = 0, count = json_object_array_length(content); i < count; ++i) {
			struct json_object *block = jb_at(content, i);
			struct cstr_view kind = cli_client_text(jb_get(block, "type"));
			if (!kind.data)
				return cli_error_set(error, EPROTO, "Malformed tool content");
			int e;
			if (cstr_view_equal(kind, CSTR_VIEW("text"))) {
				struct cstr_view text = cli_client_text(jb_get(block, "text"));
				if (!text.data)
					return cli_error_set(error, EPROTO, "Malformed tool content");
				e = cli_ui_message(cli->console, text);
			} else {
				dstr_auto label = dstr(nullptr);
				if (!dstr_cat(&label, "[") || !dstr_cat_n(&label, kind.data, kind.len) || !dstr_cat(&label, " content]"))
					return ENOMEM;
				e = cli_ui_message(cli->console, cstr_view_dstr(&label));
			}
			if (e)
				return e;
		}
		return 0;
	}
	struct json_object *structured = jb_get(result, "structuredContent");
	if (!structured)
		return cli_error_set(error, EPROTO, "AskAI returned no tool content");
	struct cstr_view wire;
	int e = cli_client_wire(structured, JSON_C_TO_STRING_PRETTY, &wire, error);
	return e ? e : cli_ui_message(cli->console, wire);
}

static int
cli_tool_call (struct cli       *cli,
               struct cstr_view  command,
               struct cli_error *error)
{
	struct autocomplete_tool const *tool = nullptr;
	cli_json_auto arguments = nullptr;
	struct autocomplete_error diagnostic = {error->message, sizeof error->message, 0};
	int e = autocomplete_parse_call(cli->client.catalog, command,
	                                 &tool, &arguments, &diagnostic);
	if (e)
		return e;
	struct cstr_view name = tool->name;
	if (cstr_view_equal(name, cstr_view_dstr(&cli->client.notes_tool))) {
		struct json_object *action = jb_get(arguments, "action");
		cli->client.notes_dirty |= cli_client_is(action, "add") || cli_client_is(action, "copy");
	}
	cli_json_auto params = jb_object("name", cli_client_string(name), "arguments", json_object_get(arguments),
	                                "_meta", jb_object("progressToken", json_object_new_string("tool"), nullptr), nullptr);
	if (!params)
		return ENOMEM;
	dstr_auto action = dstr(nullptr);
	if (!dstr_cat(&action, "call ") || !dstr_cat_n(&action, name.data, name.len))
		return ENOMEM;
	struct cli_stream stream = {.ui = cli_ui(cli->console), .text = DSTR_INIT,
	                            .actor = name, .token = CSTR_VIEW("tool")};
	e = cli_ui_action(&stream.ui, CSTR_VIEW("you"), cstr_view_dstr(&action));
	cli_json_auto result = nullptr;
	if (!e)
		e = cli_client_rpc(&cli->client, "tools/call", json_object_get(params), cli_tool_notify, &stream, &result, error);
	if (!e)
		e = cli_tool_content(cli, result, error);
	if (!e && json_object_get_boolean(jb_get(result, "isError")))
		e = cli_error_set(error, EIO, "Tool %.*s failed", (int)name.len, name.data);
	if (!e)
		e = cli_ui_action(&stream.ui, name, CSTR_VIEW("completed"));
	cli_ui_fini(&stream.ui);
	dstr_cleanup(&stream.text);
	return e;
}

static int
cli_history_show (struct cli *cli)
{
	int e = 0;
	struct dstr label = DSTR_INIT;

	do {
		if (!dstr_cat(&label, dstr_get(&cli->client.tool)) || !dstr_cat(&label, ":")) {
			e = ENOMEM;
			break;
		}

		for (size_t i = 0, count = json_object_array_length(cli->history); i < count; ++i) {
			struct json_object *turn = jb_at(cli->history, i);
			e = cli_ui_info(cli->console, CSTR_VIEW("you:"));
			if (e)
				break;

			e = cli_ui_message(cli->console, cli_client_text(jb_get(turn, "prompt")));
			if (e)
				break;

			e = cli_ui_info(cli->console, cstr_view_dstr(&label));
			if (e)
				break;

			e = cli_ui_message(cli->console, cli_client_text(jb_get(turn, "answer")));
			if (e)
				break;
		}
	} while (0);

	dstr_cleanup(&label);
	return e ? e : cli_console_flush(cli->console);
}

static int
cli_names (struct cli *cli)
{
	size_t count = autocomplete_participant_count(cli->client.catalog);
	if (!count)
		return cli_ui_message(cli->console, CSTR_VIEW("No participants discovered. /refresh reloads the shared notebooks."));
	dstr_auto names = dstr(nullptr);
	if (!dstr_cat(&names, "Known session participants:"))
		return ENOMEM;
	for (size_t i = 0; i < count; ++i) {
		struct cstr_view name = autocomplete_participant_at(cli->client.catalog, i);
		if (!dstr_cat(&names, " ") || !dstr_cat_n(&names, name.data, name.len))
			return ENOMEM;
	}
	return cli_ui_message(cli->console, cstr_view_dstr(&names));
}

enum cli_command_action { CLI_COMMAND_PROMPT, CLI_COMMAND_CONTINUE, CLI_COMMAND_EXIT, CLI_COMMAND_REGENERATE };

static int cli_command(struct cli *cli, struct dstr *prompt, enum cli_command_action *action,
                       struct cli_error *error)
{
	struct cstr_view command = cli_trim(cstr_view_dstr(prompt));
	size_t split = 0;
	while (split < command.len && !strchr(" \t\r\n", command.data[split]))
		++split;
	struct cstr_view verb = cstr_view_slice(command, 0, split);
	struct cstr_view rest = cli_trim(cstr_view_slice(command, split, SIZE_MAX));
	*action = CLI_COMMAND_CONTINUE;
	if (cstr_view_equal(command, CSTR_VIEW("/exit")) || cstr_view_equal(command, CSTR_VIEW("/quit"))) {
		*action = CLI_COMMAND_EXIT;
		return 0;
	}
	if (cstr_view_equal(command, CSTR_VIEW("/help")))
		return cli_ui_message(cli->console, CSTR_VIEW(
			"/clear starts a new conversation; /regen retries the last turn;\n"
			"/history displays the completed conversation.\n"
			"/read FILE sends a text file; /exit leaves the chat.\n"
			"/tools [NAME] lists accessible tools or shows a tool's schema.\n"
			"/tool NAME key=VALUE ... calls a tool directly; {JSON} is also accepted.\n"
			"/names lists known session participants; /refresh updates discovery.\n"
			"Tab completes tools, parameters, enum values, @names and leading nicknames.\n"
			"PgUp/PgDn scroll; Ctrl-End follows output; Ctrl-R searches input history.\n"
			"End a line with \\ to continue it, or / to submit multiline input.\n"
			"Ctrl-C cancels an answer or tool call. Direct tool results stay out of chat history."));
	if (cstr_view_equal(verb, CSTR_VIEW("/tools")))
		return cli_tools(cli, rest, error);
	if (cstr_view_equal(command, CSTR_VIEW("/names")))
		return cli_names(cli);
	if (cstr_view_equal(command, CSTR_VIEW("/refresh"))) {
		int e = cli_client_refresh(&cli->client, error);
		return e ? e : cli_ui_info(cli->console, CSTR_VIEW("Tools and session participants refreshed."));
	}
	if (cstr_view_equal(verb, CSTR_VIEW("/tool"))) {
		if (cli->options->single)
			*action = CLI_COMMAND_EXIT;
		return cli_tool_call(cli, command, error);
	}
	if (cstr_view_equal(command, CSTR_VIEW("/history")))
		return cli_history_show(cli);
	if (cstr_view_equal(command, CSTR_VIEW("/clear"))) {
		size_t count = json_object_array_length(cli->history);
		if (count)
			(void)json_object_array_del_idx(cli->history, 0, count);
		int e = cli->saved ? cli_save_error(cli, cli_history_update(cli, CLI_HISTORY_CLEAR, (struct cstr_view){}, (struct cstr_view){}, false), error) : 0;
		return e || !cli->tty ? e : cli_ui_info(cli->console, CSTR_VIEW("Conversation cleared."));
	}
	if (cstr_view_equal(command, CSTR_VIEW("/regen"))) {
		size_t count = json_object_array_length(cli->history);
		if (!count)
			return cli_error_set(error, EINVAL, "There is no previous answer to regenerate");
		struct cstr_view previous = cli_client_text(jb_get(jb_at(cli->history, count - 1), "prompt"));
		dstr_clear(prompt);
		if (!dstr_cat_n(prompt, previous.data, previous.len))
			return ENOMEM;
		*action = CLI_COMMAND_REGENERATE;
		return 0;
	}
	*action = CLI_COMMAND_PROMPT;
	if (cstr_view_equal(verb, CSTR_VIEW("/read"))) {
		if (!rest.len)
			return cli_error_set(error, EINVAL, "Usage: /read FILE");
		return cli_file_read(cli, prompt, rest, error);
	}
	return 0;
}

static int cli_turn(struct cli *cli, struct dstr *prompt, bool interpret,
                    enum cli_command_action *action, struct cli_error *error)
{
	if (cli->saved) {
		struct cstr_view input = cstr_view_dstr(prompt);
		if (interpret && input.len && input.data[input.len - 1] == '\n')
			--input.len;
		int e = cli_save_error(cli, cli_history_update(cli, CLI_HISTORY_INPUT, input, (struct cstr_view){}, false), error);
		if (e)
			return e;
	}
	*action = CLI_COMMAND_PROMPT;
	if (interpret) {
		int e = cli_command(cli, prompt, action, error);
		if (e || *action == CLI_COMMAND_CONTINUE || *action == CLI_COMMAND_EXIT)
			return e;
	}
	struct cstr_view text = cstr_view_dstr(prompt);
	if (!cli_trim(text).len) {
		*action = CLI_COMMAND_CONTINUE;
		return cli->options->single ? cli_error_set(error, EINVAL, "The prompt is empty") : 0;
	}
	bool regenerate = *action == CLI_COMMAND_REGENERATE;
	size_t count = json_object_array_length(cli->history);
	cli_json_auto answer = nullptr;
	int e = cli_ask(cli, text, count - regenerate, &answer, error);
	if (e)
		return e;
	struct json_object *turn = jb_object("prompt", cli_client_string(text), "answer", json_object_get(answer), nullptr);
	if (!turn)
		return ENOMEM;
	/* Allocate the replacement before changing history, preserving old turns
	 * if allocation or the request fails. */
	if (json_object_array_put_idx(cli->history, count - regenerate, turn)) {
		json_object_put(turn);
		turn = nullptr;
		return ENOMEM;
	}
	if (cli->saved)
		e = cli_save_error(cli, cli_history_update(cli, CLI_HISTORY_TURN, text, cli_client_text(answer), regenerate), error);
	if (cli->options->single)
		*action = CLI_COMMAND_EXIT;
	return e;
}

static int
cli_chat (struct cli       *cli,
          struct cli_error *error)
{
	bool initial = cli->options->has_prompt;
	for (;;) {
		if (cli->exit_signal || cli->ui_error)
			return cli->ui_error ? cli->ui_error : EINTR;
		error->message[0] = '\0';
		bool catalog = cli->client.catalog_dirty;
		bool refresh = catalog || cli->client.notes_dirty;
		int e = 0;
		if (refresh) {
			cli->busy = true;
			e = cli_console_busy(cli->console, true);
			if (!e)
				e = catalog ? cli_client_refresh(&cli->client, error)
				            : cli_client_refresh_notes(&cli->client, error);
			cli->busy = false;
			int display = cli_console_busy(cli->console, false);
			if (!e)
				e = display;
		}
		if (e == EINTR && !cli->exit_signal && !cli->options->single) {
			atomic_store(&cli->cancel_io, false);
			e = 0;
		}
		if (e == EINTR || e == ENOMEM || cli->client.identity_changed)
			return e;
		if (e) {
			e = cli_ui_error(cli->console, catalog ? CSTR_VIEW("Cannot refresh completion metadata") : CSTR_VIEW("Cannot refresh note filenames"),
			                 cli_error_text(error, e));
			if (e)
				return e;
			error->message[0] = '\0';
		}
		bool interpret = !initial;
		dstr_auto prompt = dstr(nullptr);
		if (initial) {
			struct cstr_view text = cstr_view_dstr(&cli->options->prompt);
			if (cli->options->file) {
				e = cli_file_read(cli, &prompt, cstr_view(cli->options->file), error);
				if (e)
					return e;
			} else if (!dstr_cat_n(&prompt, text.data, text.len)) {
				return ENOMEM;
			}
			initial = false;
		} else {
			bool eof;
			e = cli_console_read(cli->console, &prompt, cli->options->multiline, &eof, cli->tty ? "\n> " : "");
			if (e || eof)
				return e;
		}
		enum cli_command_action action;
		cli->busy = true;
		e = cli_console_busy(cli->console, true);
		if (!e)
			e = cli_turn(cli, &prompt, interpret, &action, error);
		cli->busy = false;
		int display = cli_console_busy(cli->console, false);
		if (!e)
			e = display;
		if (cli->client.identity_changed)
			return e;
		if (e == EINTR) {
			if (cli->options->single || cli->exit_signal)
				return e;
			atomic_store(&cli->cancel_io, false);
			e = cli_ui_info(cli->console, CSTR_VIEW("Cancelled."));
			if (e)
				return e;
			continue;
		}
		if (e) {
			if (cli->options->single || !cli->tty || e == ENOMEM || ferror(stdout))
				return e;
			e = cli_ui_error(cli->console, cli_error_text(error, e), (struct cstr_view){});
			if (e)
				return e;
		} else if (action == CLI_COMMAND_EXIT) {
			return 0;
		}
	}
}

static int
cli_run (struct cli_options const *options,
         struct cli_error         *error,
         unsigned                 *exit_signal)
{
	struct cli *cli = calloc(1, sizeof *cli);
	if (!cli)
		return ENOMEM;
	atomic_init(&cli->cancel_io, false);
	cli->options = options;
	cli->tty = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
	cli->signals.ref.fd = -1;
	cli->mailbox.wake.fd = -1;
	int e = cli_loop_init(&cli->reactor);
	if (!e)
		e = cli_signals_init(&cli->signals, &cli->reactor, cli_signal);
	bool curl_ready = false;
	if (!e) {
		e = curl_global_init(CURL_GLOBAL_DEFAULT) ? EIO : 0;
		curl_ready = !e;
	}
	if (!e)
		e = cli_mailbox_init(&cli->mailbox, &cli->reactor, (struct cli_mailbox_limits){
			.messages = 256, .bytes = 1 << 20, .control_messages = 64, .control_bytes = 64 << 10
		});
	if (!e)
		cli->network = cli_network_create(&cli->reactor, &cli->mailbox, &e);
	if (!e)
		cli->io = cli_io_create(cli_network_loop(cli->network), &cli->mailbox, &e);
	if (!e)
		cli->client = cli_client(options->socket, options->tool, cli->network, &e);
	bool color = !strcmp(options->color, "on") || (!strcmp(options->color, "auto") && cli->tty && !getenv("NO_COLOR"));
	if (!e)
		cli->console = cli_console_create(options->simple || !cli->tty, color, &cli->reactor, cli->io, &e);
	if (!e) {
		cli->client.report = cli_report;
		cli->reactor.after_turn = cli_service;
		cli_network_set_ready(cli->network, cli_output_ready, cli);
		e = cli_client_connect(&cli->client, error);
	}
	if (!e)
		cli->files = autocomplete_file_create(&e);
	if (!e) {
		cli->history = json_object_new_array();
		if (!cli->history)
			e = ENOMEM;
	}
	if (!e)
		cli_console_set_completion(cli->console, cli_complete, cli);
	if (!e && cli->tty) {
		dstr_auto message = dstr(nullptr);
		if (!dstr_printf(&message, "Connected to %s. /help lists commands; Ctrl-C cancels an answer.", dstr_get(&cli->client.tool)))
			e = ENOMEM;
		else
			e = cli_ui_info(cli->console, cstr_view_dstr(&message));
	}
	if (!e)
		e = cli_history_open(cli, error);
	if (!e)
		e = cli_chat(cli, error);
	if (!e && cli->ui_error)
		e = cli->ui_error;
	if (cli->exit_signal)
		e = EINTR;
	if (e == EPIPE)
		(void)cli_error_set(error, e, "Cannot write the client output: %s", strerror(e));
	if (!e && cli->console) {
		e = cli_console_flush(cli->console);
		while (!e && cli_console_pending(cli->console) && !cli->exit_signal) {
			e = cli_loop_turn(&cli->reactor, UINT64_MAX);
			if (!e)
				e = cli->ui_error;
		}
	}
	/* End editor activity before stopping I/O, retaining console storage
	 * until any outstanding regular-file operation has returned. */
	if (cli->console)
		(void)cli_console_suspend(cli->console);
	cli_io_stop(cli->io);
	uint64_t due = cli->exit_due ? cli->exit_due : cli_loop_clock() + 2'000'000'000;
	while (!cli_io_done(cli->io)) {
		int turn = cli_loop_turn(&cli->reactor, due);
		if (turn || cli_loop_clock() >= due)
			cli_force_exit(cli);
	}
	cli_network_stop(cli->network);
	while (!cli_network_done(cli->network)) {
		int turn = cli_loop_turn(&cli->reactor, due);
		if (turn || cli_loop_clock() >= due)
			cli_force_exit(cli);
	}
	cli_network_destroy(&cli->network);
	cli_console_destroy(&cli->console);
	cli_io_destroy(&cli->io);
	cli_state_destroy(&cli->saved);
	autocomplete_file_destroy(&cli->files);
	cli_client_json_cleanup(&cli->history);
	cli_client_fini(&cli->client);
	cli_mailbox_fini(&cli->mailbox);
	cli_signals_fini(&cli->signals);
	cli_loop_fini(&cli->reactor);
	if (curl_ready)
		curl_global_cleanup();
	if (cli->exit_signal)
		e = EINTR;
	if (e == EPIPE && !error->message[0])
		(void)cli_error_set(error, e, "Cannot write the client output: %s", strerror(e));
	*exit_signal = cli->exit_signal;
	free(cli);
	cli = nullptr;
	return e;
}

int
main (int    argc,
      char **argv)
{
	(void)setlocale(LC_ALL, "");
	struct cli_options options = {.prompt = DSTR_INIT, .color = "auto", .reasoning = true};
	struct cli_error error = {};
	unsigned exit_signal = 0;
	int e = cli_args(&options, argc, argv, &error);
	if (!e && !options.done)
		e = cli_run(&options, &error, &exit_signal);
	dstr_cleanup(&options.prompt);
	if (e == EINTR && exit_signal)
		return 128 + (int)exit_signal;
	if (e) {
		(void)cli_console_write_error(CSTR_VIEW("askai-cli: "));
		(void)cli_console_write_error(cli_error_text(&error, e));
		(void)cli_console_write_error(CSTR_VIEW("\n"));
	}
	return e ? 1 : 0;
}
