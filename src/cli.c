/** @file cli.c
 *
 * Unix-socket MCP chat client. Inference, tools and notes belong to AskAI.
 */
#define _GNU_SOURCE

#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "autocomplete_file.h"
#include "buf.h"
#include "child.h"
#include "cli_client.h"
#include "cli_command.h"
#include "cli_console.h"
#include "cli_io.h"
#include "cli_loop.h"
#include "cli_mailbox.h"
#include "cli_network.h"
#include "cli_signals.h"
#include "cli_state.h"
#include "cli_stream.h"
#include "cli_tool.h"
#include "cli_ui.h"
#include "compat.h"
#include "dstr.h"
#include "jb.h"
#include "list.h"
#include "loop.h"

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
	if (!path.len || !path.data ||
	    memchr(path.data, 0, path.len))
		return EINVAL;

	dstr_auto expanded = dstr(nullptr);
	dstr_auto contents = dstr(nullptr);
	__attribute__((cleanup(buf_fini)))
	struct buf bytes = {};
	char const *home = getenv("HOME");
	if (home && (cstr_view_equal(path, CSTR_VIEW("~")) ||
	             cstr_view_starts(path, CSTR_VIEW("~/")))) {
		if (!dstr_cat(&expanded, home) ||
		    !dstr_cat_n(&expanded, path.data + 1, path.len - 1))
			return ENOMEM;
	} else if (!dstr_cat_n(&expanded, path.data, path.len)) {
		return ENOMEM;
	}

	FILE *file = fopen(dstr_get(&expanded), "rb");
	int e = 0;
	if (!file) {
		e = errno;
		return cli_error_set(error, e, "%s: %s",
		                     dstr_get(&expanded),
		                     strerror(e));
	}
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
	if (!e && result.len && (memchr(result.data, 0, result.len) ||
	                         jb_utf8(result.data, result.len) != result.len))
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
cli_commands_text (struct dstr *text)
{
	for (unsigned i = 0; i < CLI_COMMAND_COUNT; ++i) {
		struct cli_command const *command = cli_command_at(i);
		if (!dstr_cat_n(text, command->usage.data, command->usage.len) ||
		    !dstr_cat(text, "  ") ||
		    !dstr_cat_n(text, command->description.data, command->description.len) ||
		    !dstr_cat(text, "\n"))
			return ENOMEM;
	}
	if (!dstr_cat(text, "Tab completes; press twice to list choices.\n"
	                   "PgUp/PgDn search history; Alt-Up/Down or Ctrl-PgUp/PgDn scroll.\n"
	                   "Alt-Left/Right selects a pane; Ctrl-End follows; Ctrl-R searches history.\n"
	                   "End a line with \\ to continue it, or / to submit multiline input.\n"
	                   "Ctrl-C cancels an answer or tool call. Direct tool results stay out of chat history."))
		return ENOMEM;
	return 0;
}

static int
cli_help (void)
{
	if (puts("Usage: askai-cli --socket PATH [options]\n"
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
	         "Commands:") < 0)
		return EIO;
	dstr_auto text = dstr(nullptr);
	int e = cli_commands_text(&text);
	if (e)
		return e;
	struct cstr_view output = cstr_view_dstr(&text);
	return fwrite(output.data, 1, output.len, stdout) == output.len && putchar('\n') != EOF ? 0 : EIO;
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
	struct list               inputs;
	struct list               tools;
	size_t                    input_bytes;
	uint64_t                  exit_due;
	unsigned                  exit_signal;
	int                       ui_error;
	bool                      busy;
	bool                      tty;
	bool                      quitting;
	_Atomic bool              cancel_io;
};

enum { CLI_INPUT_HANDLED = 1u << 0 };

struct cli_input {
	struct list hook;
	struct dstr text;
	size_t      flags;
};

static int cli_immediate (struct cli *cli);

static void
cli_shutdown (struct cli *cli)
{
	if (!cli->exit_due)
		cli->exit_due = loop_clock() + 2'000'000'000;
	cli->reactor.until = cli->exit_due;
	atomic_store(&cli->cancel_io, true);
	cli_network_cancel(cli->network, true);
	if (cli->console)
		(void)cli_console_interrupt(cli->console, true);
}

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
				if (cli->busy)
					atomic_store(&cli->cancel_io, true);
				else
					cli_tool_cancel_last(&cli->tools);
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
		cli_shutdown(cli);
		break;
	}
	if (e && !cli->ui_error)
		cli->ui_error = e;
}

/** A wait may leave a stuck syscall to process exit, but must never unwind
 * storage that its worker still owns. Restore the terminal without freeing
 * any of those objects, then let the kernel reclaim the entire process.
 */
static void
cli_force_exit (struct cli *cli)
{
	if (cli->console)
		(void)cli_console_suspend(cli->console);
	_Exit(cli->exit_signal ? 128 + (int)cli->exit_signal : cli->quitting && !cli->ui_error ? 0 : 1);
}

/** Run a reactor turn until due; exit at once after a failure or past due. */
static void
cli_wait (struct cli *cli,
          uint64_t    due)
{
	if (cli_loop_turn(&cli->reactor, due) || loop_clock() >= due)
		cli_force_exit(cli);
}

static void
cli_service (struct cli_loop *loop)
{
	struct cli *cli = container_of(loop, struct cli, reactor);
	if (cli->exit_due && loop_clock() >= cli->exit_due)
		cli_force_exit(cli);
	cli_network_service(cli->network);
	if (cli->console) {
		int e = cli_console_service(cli->console);
		if (!e && !cli->quitting)
			e = cli_immediate(cli);
		if (e && e != EINTR && !cli->ui_error)
			cli->ui_error = e;
	}
	if (cli->ui_error)
		cli_shutdown(cli);
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
	if (cli->quitting)
		return 0;
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
	if (cli->exit_signal || cli->quitting)
		return EINTR;
	struct cli_work work = {.job = {.work = cli_work_run, .done = cli_work_done},
	                        .fn = fn, .data = data};
	int e = cli_io_submit(cli->io, &work.job);
	if (e)
		return e;
	bool busy = cli->busy;
	cli->busy = true;
	while (!work.done)
		cli_wait(cli, cli->exit_due ? cli->exit_due : UINT64_MAX);
	cli->busy = busy;
	if (!busy && !cli->exit_signal && !cli->quitting)
		atomic_store(&cli->cancel_io, false);
	return cli->exit_signal || cli->quitting ? EINTR : work.job.error;
}

enum cli_history_operation { CLI_HISTORY_OPEN, CLI_HISTORY_INPUT, CLI_HISTORY_TURN, CLI_HISTORY_CLEAR };

struct cli_history_work {
	struct cli_state           *state;
	struct cstr_view            path;
	struct cstr_view            text;
	struct cstr_view            answer;
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

/** Save an input, without the newline that ends it if it is a line. */
static int
cli_history_input (struct cli       *cli,
                   struct cstr_view  input,
                   bool              line,
                   struct cli_error *error)
{
	if (line && input.len && input.data[input.len - 1] == '\n')
		--input.len;
	return cli_save_error(cli, cli_history_update(cli, CLI_HISTORY_INPUT, input, (struct cstr_view){}, false), error);
}

/** Mark the client busy or idle; manual tool calls keep it shown busy. */
static int
cli_busy (struct cli *cli,
          bool        busy)
{
	cli->busy = busy;
	return cli_console_busy(cli->console, busy || !list_is_empty(&cli->tools));
}

struct cli_completion_work {
	struct autocomplete_file        *files;
	struct cstr_view                 line;
	struct autocomplete_match const *matches;
	size_t                           cursor;
	size_t                           count;
};

static int
cli_completion_work (void *data)
{
	struct cli_completion_work *work = data;
	return autocomplete_file_complete(work->files, work->line, work->cursor,
	                                  cli_command_at(CLI_COMMAND_READ)->completion, &work->matches, &work->count);
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
	int e = dstr_cat(work->identity, "socket=") &&
	        dstr_cat(work->identity, path) ? 0 : ENOMEM;
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
	cli_json_auto params = jb_object("name", cli_client_string(cstr_view_dstr(&cli->client.tool)),
	                                 "arguments", json_object_get(args),
	                                 "_meta", jb_object("askai", jb_object("stream", json_object_new_boolean(true), nullptr),
	                                                    "progressToken", json_object_new_string("cli"), nullptr), nullptr);
	if (!params)
		return ENOMEM;
	uint64_t flags = CLI_STREAM_DELTAS;
	if (cli->options->reasoning)
		flags |= CLI_STREAM_REASONING;
	struct cli_stream stream = cli_stream(cli->console, cstr_view_dstr(&cli->client.tool),
	                                      CSTR_VIEW("cli"), flags);
	cli_json_auto result = nullptr;
	int e = cli_client_rpc(&cli->client, "tools/call", json_object_get(params),
	                       cli_stream_notify, &stream, &result, error);
	if (!e)
		e = cli_stream_result(&stream, result, answer, error);
	cli_stream_fini(&stream);
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
		summary.len = jb_utf8(summary.data, summary.len < 160 ? summary.len : 160);
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

static void
cli_manual_done (struct cli_tool        *tool,
                 void                   *context,
                 int                     e,
                 struct json_object     *result,
                 struct cli_error const *error)
{
	struct cli *cli = context;
	if (!cli->quitting && !cli->exit_signal) {
		e = cli_tool_finish(tool, e, result, error);
		if (e && !cli->ui_error)
			cli->ui_error = e;
		if (cli->client.notes_dirty || cli->client.catalog_dirty)
			cli_console_yield(cli->console);
	}
	e = cli_busy(cli, cli->busy);
	if (e && !cli->ui_error)
		cli->ui_error = e;
}

static int
cli_manual_call (struct cli       *cli,
                 struct cstr_view  command,
                 struct cli_error *error)
{
	int e;
	struct cli_tool *tool = cli_tool_create(&cli->client, cli->console, command, error, &e);
	if (!tool)
		return e;
	if (cli->tty && !cli->options->simple && !cli->options->single) {
		e = cli_tool_submit(tool, &cli->tools, cli_manual_done, cli, error);
		if (!e) {
			tool = nullptr;
			e = cli_console_busy(cli->console, true);
		}
	} else {
		e = cli_tool_run(tool, error);
	}
	cli_tool_destroy(&tool);
	return e;
}

static int
cli_history_show (struct cli *cli)
{
	dstr_auto label = dstr(nullptr);
	if (!dstr_printf(&label, "%s:", dstr_get(&cli->client.tool)))
		return ENOMEM;
	for (size_t i = 0, count = json_object_array_length(cli->history); i < count; ++i) {
		struct json_object *turn = jb_at(cli->history, i);
		int e = cli_ui_info(cli->console, CSTR_VIEW("you:"));
		if (!e)
			e = cli_ui_message(cli->console, cli_client_text(jb_get(turn, "prompt")));
		if (!e)
			e = cli_ui_info(cli->console, cstr_view_dstr(&label));
		if (!e)
			e = cli_ui_message(cli->console, cli_client_text(jb_get(turn, "answer")));
		if (e)
			return e;
	}
	return 0;
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

enum cli_action { CLI_ACTION_PROMPT, CLI_ACTION_CONTINUE, CLI_ACTION_EXIT, CLI_ACTION_REGENERATE };

static struct cli_command const *
cli_command_parse (struct cstr_view  line,
                   struct cstr_view *rest)
{
	line = cli_trim(line);
	size_t split = 0;
	while (split < line.len && !strchr(" \t\r\n", line.data[split]))
		++split;
	*rest = cli_trim(cstr_view_slice(line, split, SIZE_MAX));
	struct cli_command const *command = cli_command_find(cstr_view_slice(line, 0, split));
	return command && (command->args != CLI_COMMAND_ARGS_NONE || !rest->len) ? command : nullptr;
}

static int
cli_commands_help (struct cli *cli)
{
	dstr_auto text = dstr(nullptr);
	int e = cli_commands_text(&text);
	return e ? e : cli_ui_message(cli->console, cstr_view_dstr(&text));
}

static int
cli_command_run (struct cli               *cli,
                 struct dstr              *prompt,
                 struct cli_command const *command,
                 struct cstr_view          rest,
                 enum cli_action          *action,
                 struct cli_error         *error)
{
	*action = CLI_ACTION_PROMPT;
	if (!command)
		return 0;
	*action = CLI_ACTION_CONTINUE;
	switch (command->id) {
	case CLI_COMMAND_EXIT:
	case CLI_COMMAND_QUIT:
		*action = CLI_ACTION_EXIT;
		return 0;
	case CLI_COMMAND_HELP:
		return cli_commands_help(cli);
	case CLI_COMMAND_TOOLS:
		return cli_tools(cli, rest, error);
	case CLI_COMMAND_NAMES:
		return cli_names(cli);
	case CLI_COMMAND_REFRESH: {
		int e = cli_client_refresh(&cli->client, error);
		return e ? e : cli_ui_info(cli->console, CSTR_VIEW("Tools and session participants refreshed."));
	}
	case CLI_COMMAND_TOOL:
		if (cli->options->single)
			*action = CLI_ACTION_EXIT;
		return cli_manual_call(cli, cli_trim(cstr_view_dstr(prompt)), error);
	case CLI_COMMAND_HISTORY:
		return cli_history_show(cli);
	case CLI_COMMAND_CLEAR: {
		size_t count = json_object_array_length(cli->history);
		if (count)
			(void)json_object_array_del_idx(cli->history, 0, count);
		int e = cli->saved ? cli_save_error(cli, cli_history_update(cli, CLI_HISTORY_CLEAR, (struct cstr_view){}, (struct cstr_view){}, false), error) : 0;
		return e || !cli->tty ? e : cli_ui_info(cli->console, CSTR_VIEW("Conversation cleared."));
	}
	case CLI_COMMAND_REGEN: {
		size_t count = json_object_array_length(cli->history);
		if (!count)
			return cli_error_set(error, EINVAL, "There is no previous answer to regenerate");
		struct cstr_view previous = cli_client_text(jb_get(jb_at(cli->history, count - 1), "prompt"));
		*action = CLI_ACTION_REGENERATE;
		return cstr_view_store(previous, prompt);
	}
	case CLI_COMMAND_READ:
		*action = CLI_ACTION_PROMPT;
		if (!rest.len)
			return cli_error_set(error, EINVAL, "Usage: %.*s", (int)command->usage.len, command->usage.data);
		return cli_file_read(cli, prompt, rest, error);
	default:
		return EINVAL;
	}
}

static bool
cli_immediate_command (void             *ctx,
                       struct cstr_view  line)
{
	struct cli *cli = ctx;
	if (cli->quitting)
		return false;
	struct cstr_view rest;
	struct cli_command const *command = cli_command_parse(line, &rest);
	return command && (command->id == CLI_COMMAND_QUIT || command->id == CLI_COMMAND_EXIT ||
	                   (cli->busy && command->policy != CLI_COMMAND_WAIT));
}

static int
cli_input_append (struct cli  *cli,
                  struct dstr *text,
                  size_t       flags)
{
	size_t bytes = sizeof(struct cli_input) + dstr_len(text);
	if (bytes > CLI_LIMIT - cli->input_bytes)
		return ENOBUFS;
	struct cli_input *input = malloc(sizeof *input);
	if (!input)
		return ENOMEM;
	*input = (struct cli_input){.text = dstr_move(text), .flags = flags};
	list_append(&cli->inputs, &input->hook);
	cli->input_bytes += bytes;
	return 0;
}

static void
cli_input_destroy (struct cli_input **ptr)
{
	struct cli_input *input = *ptr;
	if (!input)
		return;
	list_del(&input->hook);
	dstr_cleanup(&input->text);
	free(input);
	*ptr = nullptr;
}

static int
cli_immediate (struct cli *cli)
{
	dstr_auto text = dstr(nullptr);
	bool ready;
	int e = cli_console_take_immediate(cli->console, &text, &ready);
	if (e || !ready)
		return e;
	struct cstr_view rest;
	struct cli_command const *command = cli_command_parse(cstr_view_dstr(&text), &rest);
	if (!command)
		return EINVAL;
	if (command->id == CLI_COMMAND_EXIT || command->id == CLI_COMMAND_QUIT) {
		cli->quitting = true;
		cli_shutdown(cli);
		return 0;
	}
	if (command->policy == CLI_COMMAND_INTERRUPT) {
		e = cli_input_append(cli, &text, 0);
		if (!e) {
			atomic_store(&cli->cancel_io, true);
			cli_network_cancel(cli->network, false);
		}
		return e;
	}
	struct cli_error error = {};
	enum cli_action action;
	e = cli_command_run(cli, &text, command, rest, &action, &error);
	if (e)
		e = cli_ui_error(cli->console, cli_error_text(&error, e), (struct cstr_view){});
	if (!e && cli->saved)
		e = cli_input_append(cli, &text, CLI_INPUT_HANDLED);
	return e;
}

static int
cli_turn (struct cli       *cli,
          struct dstr      *prompt,
          bool              interpret,
          enum cli_action  *action,
          struct cli_error *error)
{
	struct cstr_view rest = {};
	struct cli_command const *command = interpret ? cli_command_parse(cstr_view_dstr(prompt), &rest) : nullptr;
	if (command && (command->policy == CLI_COMMAND_LOCAL ||
	                (command->policy == CLI_COMMAND_CONCURRENT && cli->tty &&
	                 !cli->options->simple && !cli->options->single))) {
		int e = cli_command_run(cli, prompt, command, rest, action, error);
		if (!e && cli->saved && *action != CLI_ACTION_EXIT)
			e = cli_input_append(cli, prompt, CLI_INPUT_HANDLED);
		return e;
	}
	if (cli->saved) {
		int e = cli_history_input(cli, cstr_view_dstr(prompt), interpret, error);
		if (e)
			return e;
	}
	*action = CLI_ACTION_PROMPT;
	if (interpret) {
		int e = cli_command_run(cli, prompt, command, rest, action, error);
		if (e || *action == CLI_ACTION_CONTINUE || *action == CLI_ACTION_EXIT)
			return e;
	}
	if (atomic_load(&cli->cancel_io))
		return EINTR;
	struct cstr_view text = cstr_view_dstr(prompt);
	if (!cli_trim(text).len) {
		*action = CLI_ACTION_CONTINUE;
		return cli->options->single ? cli_error_set(error, EINVAL, "The prompt is empty") : 0;
	}
	bool regenerate = *action == CLI_ACTION_REGENERATE;
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
		*action = CLI_ACTION_EXIT;
	return e;
}

static int
cli_chat (struct cli       *cli,
          struct cli_error *error)
{
	bool initial = cli->options->has_prompt;
	for (;;) {
		if (cli->quitting)
			return 0;
		if (cli->exit_signal || cli->ui_error)
			return cli->ui_error ? cli->ui_error : EINTR;
		error->message[0] = '\0';
		dstr_auto prompt = dstr(nullptr);
		bool supplied = !list_is_empty(&cli->inputs);
		bool interpret = supplied || !initial;
		if (supplied) {
			struct cli_input *input = container_of(cli->inputs.next, struct cli_input, hook);
			cli->input_bytes -= sizeof *input + dstr_len(&input->text);
			prompt = dstr_move(&input->text);
			bool handled = input->flags & CLI_INPUT_HANDLED;
			cli_input_destroy(&input);
			if (handled) {
				int e = cli_history_input(cli, cstr_view_dstr(&prompt), true, error);
				if (e == EINTR && !cli->exit_signal && !cli->quitting) {
					atomic_store(&cli->cancel_io, false);
					e = cli_input_append(cli, &prompt, CLI_INPUT_HANDLED);
				}
				if (e)
					return cli->quitting ? 0 : e;
				continue;
			}
		}
		bool catalog = cli->client.catalog_dirty;
		bool refresh = !supplied && !cli_console_queued(cli->console) &&
		               list_is_empty(&cli->tools) && (catalog || cli->client.notes_dirty);
		int e = 0;
		if (refresh) {
			e = cli_busy(cli, true);
			if (!e)
				e = catalog ? cli_client_refresh(&cli->client, error)
				            : cli_client_refresh_notes(&cli->client, error);
			int display = cli_busy(cli, false);
			if (!e)
				e = display;
		}
		if (cli->quitting)
			return 0;
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
		if (!supplied && !list_is_empty(&cli->inputs))
			continue;
		if (!supplied && initial) {
			struct cstr_view text = cstr_view_dstr(&cli->options->prompt);
			if (cli->options->file) {
				e = cli_file_read(cli, &prompt, cstr_view(cli->options->file), error);
				if (e)
					return e;
			} else if (!dstr_cat_n(&prompt, text.data, text.len)) {
				return ENOMEM;
			}
			initial = false;
		} else if (!supplied) {
			bool eof;
			e = cli_console_read(cli->console, &prompt, cli->options->multiline, &eof, cli->tty ? "\n> " : "");
			if (e == EAGAIN)
				continue;
			if (e || eof)
				return cli->quitting ? 0 : e;
		}
		enum cli_action action;
		e = cli_busy(cli, true);
		if (!e)
			e = cli_turn(cli, &prompt, interpret, &action, error);
		int display = cli_busy(cli, false);
		if (!e)
			e = display;
		if (cli->quitting)
			return 0;
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
		} else if (action == CLI_ACTION_EXIT) {
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
	list_init(&cli->inputs);
	list_init(&cli->tools);
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
		cli_console_set_immediate(cli->console, cli_immediate_command, cli);
	if (!e)
		e = cli_chat(cli, error);
	cli->quitting = true;
	if (!list_is_empty(&cli->tools)) {
		cli_shutdown(cli);
		while (!list_is_empty(&cli->tools))
			cli_wait(cli, cli->exit_due);
	}
	if (!e && cli->ui_error)
		e = cli->ui_error;
	if (cli->exit_signal)
		e = EINTR;
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
	uint64_t due = cli->exit_due ? cli->exit_due : loop_clock() + 2'000'000'000;
	while (!cli_io_done(cli->io))
		cli_wait(cli, due);
	cli_network_stop(cli->network);
	while (!cli_network_done(cli->network))
		cli_wait(cli, due);
	cli_network_destroy(&cli->network);
	cli_console_destroy(&cli->console);
	cli_io_destroy(&cli->io);
	cli_state_destroy(&cli->saved);
	while (!list_is_empty(&cli->inputs)) {
		struct cli_input *input = container_of(cli->inputs.next, struct cli_input, hook);
		cli_input_destroy(&input);
	}
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
	if (e == EPIPE)
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
	struct cli_options options = {.prompt = DSTR_INIT, .color = "auto", .reasoning = true};
	struct cli_error error = {};
	unsigned exit_signal = 0;
	/* Before askai-cli opens any file, so that none takes such a number. */
	int e = child_std_open();
	if (e)
		(void)cli_error_set(&error, e, "Cannot open /dev/null: %s", strerror(e));
	else
		e = cli_args(&options, argc, argv, &error);
	if (!e && !options.done)
		e = cli_run(&options, &error, &exit_signal);
	dstr_cleanup(&options.prompt);
	if (e == EINTR && exit_signal)
		return 128 + (int)exit_signal;
	if (e) {
		struct cstr_view const line[] = {CSTR_VIEW("askai-cli: "), cli_error_text(&error, e), CSTR_VIEW("\n")};
		(void)cli_console_write_error(line, array_size(line));
	}
	return e ? 1 : 0;
}
