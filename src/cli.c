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
#include "cli_network.h"
#include "cli_signals.h"
#include "cli_state.h"
#include "cli_stream.h"
#include "cli_tool.h"
#include "cli_ui.h"
#include "clk.h"
#include "compat.h"
#include "dstr.h"
#include "jb.h"
#include "list.h"
#include "loop.h"
#include "util.h"
#include "xlist.h"

static int
cli_read_file (struct autocomplete_file const *files,
               struct dstr                    *text,
               struct cstr_view                path,
               struct cli_error               *error)
{
	if (!path.len || !path.data ||
	    memchr(path.data, 0, path.len))
		return EINVAL;

	dstr_auto expanded = dstr(nullptr);
	if (autocomplete_file_expand(files, path, &expanded))
		return ENOMEM;
	int fd = open(dstr_get(&expanded), O_RDONLY | O_CLOEXEC);
	/* A file read can end inside a UTF-8 character. Assemble bytes first. */
	struct buf bytes = {};
	int e = fd < 0 ? errno : buf_read(&bytes, fd, 0, CLI_LIMIT);
	util_close(&fd);
	struct span const s = buf_peek(&bytes);
	struct cstr_view const contents = { (char const *)s.ptr, s.len };
	e = e == ENOBUFS ? cli_error_set(error, EFBIG, "Prompt file exceeds the %d MiB client limit",
	                                 CLI_LIMIT >> 20)
	  : e ? cli_error_set(error, e, "%s: %s", dstr_get(&expanded), strerror(e))
	  : !jb_is_text(contents) ? cli_error_set(error, EILSEQ, "Prompt files must contain UTF-8 "
	                                          "text without NUL bytes")
	  : cstr_view_store(contents, text);
	buf_fini(&bytes);
	return e;
}

/* The values of --color: X(ID, name), the constant CLI_COLOR_ID and the
 * name.
 */
#define CLI_COLOR_LIST(X) X(AUTO, "auto") X(ON, "on") X(OFF, "off")

#define CLI_COLOR_ENUM(id, ...) CLI_COLOR_##id,
enum cli_color : uint32_t { CLI_COLOR_LIST(CLI_COLOR_ENUM) };
#undef CLI_COLOR_ENUM

/** The flags of struct cli_options: the options without an argument,
 *  whether --help or --version ran, and whether stdin and stdout are a
 *  terminal.
 */
enum {
	CLI_SINGLE       = 1u << 0,
	CLI_SIMPLE       = 1u << 1,
	CLI_MULTILINE    = 1u << 2,
	CLI_NO_REASONING = 1u << 3,
	CLI_NO_HISTORY   = 1u << 4,
	CLI_DONE         = 1u << 5,
	CLI_TTY          = 1u << 6,
};

struct cli_options {
	char const     *socket;
	char const     *file;
	char const     *tool;
	char const     *system;
	char const     *history_repo;
	char const     *session_id;
	char const     *prompt;
	uint32_t        flags;
	enum cli_color  color;
};

/* The help of the commands and of the editing keys, for --help and /help. */
#define CLI_COMMAND_LINE(id, word, args, policy, syntax, description) \
        "/" #word syntax "  " description "\n"
static struct cstr_view const cli_commands_text = CSTR_VIEW_INIT(
	CLI_COMMAND_LIST(CLI_COMMAND_LINE)
	"Tab completes; press twice to list choices.\n"
	"PgUp/PgDn search history; Alt-Up/Down or Ctrl-PgUp/PgDn scroll.\n"
	"Alt-Left/Right selects a pane; Ctrl-End follows; Ctrl-R searches history.\n"
	"End a line with \\ to continue it, or / to submit multiline input.\n"
	"Ctrl-C cancels an answer or tool call. Direct tool results stay out of chat history.");
#undef CLI_COMMAND_LINE

/* The options that the help lists: X(name, key, ARG, flag, help, short),
 * the long name, the value that getopt_long() returns, the name of the
 * argument, or "" for an option without one, the CLI_ flag that an option
 * without an argument sets, or 0, the help, and the short form, if the key
 * is also one.
 */
#define CLI_OPTION_LIST(X)                                                                                   \
        X("socket",          's', "PATH", 0,                "AskAI socket (or ASKAI_SOCKET)")                \
        X("tool",            't', "NAME", 0,                "Select the configured ask tool")                \
        X("prompt",          'p', "TEXT", 0,                "Send an initial prompt", "p")                   \
        X("file",            'f', "FILE", 0,                "Read the initial prompt from a text file", "f") \
        X("system",          'S', "TEXT", 0,                "Override server system instructions")           \
        X("single-turn",     '1', "",     CLI_SINGLE,       "Exit after one answer")                         \
        X("simple-io",       'i', "",     CLI_SIMPLE,       "Basic line input; automatic for pipes")         \
        X("multiline-input", 'm', "",     CLI_MULTILINE,    "Enter adds a line; finish with a trailing /")   \
        X("color",           'c', XL_JOIN("|", "|", CLI_COLOR_LIST(XL_TEXT)),                                \
                                          0,                "Terminal colors (default auto)")                \
        X("no-reasoning",    'r', "",     CLI_NO_REASONING, "Hide exposed reasoning")                        \
        X("history-repo",    'H', "PATH", 0,                "Separate Git repository for client history")    \
        X("session-id",      'I', "ID",   0,                "Override the saved-history session identity")   \
        X("no-history",      'N', "",     CLI_NO_HISTORY,   "Disable saved client history")                  \
        X("help",            'h', "",     0,                "Show this help", "h")

/* The help of an option starts in column 28. */
static int
cli_help (void)
{
	#define CLI_OPTION_HELP(name, key, meta, flag, help, ...)                                 \
	        printf("  %-25s" help "\n", __VA_OPT__("-" __VA_ARGS__ ", ") "--" name " " meta) < 0 ||
	return puts("Usage: askai-cli --socket PATH [options]\n"
	            "Chat with AskAI's upstream LLM over its Unix socket.\n") < 0 ||
	       CLI_OPTION_LIST(CLI_OPTION_HELP)
	       printf("\nCommands:\n%s\n", cli_commands_text.data) < 0 ? EIO : 0;
	#undef CLI_OPTION_HELP
}

static int
cli_args (struct cli_options  *out,
          int                  argc,
          char               **argv,
          struct cli_error    *error)
{
	out->socket = getenv("ASKAI_SOCKET");
	#define CLI_OPTION_ENTRY(name, key, meta, ...) \
	        {name, sizeof meta > 1 ? required_argument : no_argument, nullptr, key},
	static struct option const options[] = {
		CLI_OPTION_LIST(CLI_OPTION_ENTRY)
		{"version", no_argument, nullptr, 'v'},
		{}
	};
	#undef CLI_OPTION_ENTRY
	#define CLI_OPTION_FLAG(name, key, meta, flag, ...) [key] = flag,
	static uint32_t const flags[128] = {CLI_OPTION_LIST(CLI_OPTION_FLAG)};
	#undef CLI_OPTION_FLAG
	static struct cstr_view const colors[] = { CLI_COLOR_LIST(XL_VIEW) };
	size_t color = CLI_COLOR_AUTO;
	size_t prompts = 0;
	int c;
	while ((c = getopt_long(argc, argv, "hp:f:", options, nullptr)) != -1) {
		if (c < (int)array_size(flags) && flags[c]) {
			out->flags |= flags[c];
			continue;
		}
		switch (c) {
		case 's': out->socket = optarg; break;
		case 't': out->tool = optarg; break;
		case 'p': out->prompt = optarg; ++prompts; break;
		case 'f': out->file = optarg; ++prompts; break;
		case 'S': out->system = optarg; break;
		case 'c': color = cstr_view_index(colors, array_size(colors), cstr_view(optarg)); break;
		case 'H': out->history_repo = optarg; break;
		case 'I': out->session_id = optarg; break;
		case 'h': out->flags |= CLI_DONE; return cli_help();
		case 'v': out->flags |= CLI_DONE; return puts("askai-cli 1") < 0 ? EIO : 0;
		default: return cli_error_set(error, EINVAL, "Invalid option; see --help");
		}
	}
	struct cstr_view const scheme = CSTR_VIEW("unix:");
	if (cstr_view_starts(cstr_view(out->socket), scheme))
		out->socket += scheme.len;
	if (optind != argc)
		return cli_error_set(error, EINVAL, "Unexpected positional argument; see --help");
	if (prompts > 1)
		return cli_error_set(error, EINVAL, "Use only one --prompt or --file");
	if (out->session_id && !*out->session_id)
		return cli_error_set(error, EINVAL, "--session-id requires a nonempty identity");
	if (!out->socket || !*out->socket)
		return cli_error_set(error, EINVAL, "Specify --socket PATH or ASKAI_SOCKET");
	if ((out->flags & CLI_NO_HISTORY) && out->history_repo && *out->history_repo)
		return cli_error_set(error, EINVAL, "--history-repo and --no-history are mutually exclusive");
	if (color == array_size(colors))
		return cli_error_set(error, EINVAL, "--color must be "
		                     XL_JOIN(", ", " or ", CLI_COLOR_LIST(XL_TEXT)));
	out->color = color;
	out->flags |= isatty(STDIN_FILENO) && isatty(STDOUT_FILENO) ? CLI_TTY : 0;
	return 0;
}

#undef CLI_OPTION_LIST
#undef CLI_COLOR_LIST

struct cli {
	struct cli_loop           reactor;
	struct cli_client         client;
	struct cli_signals        signals;
	struct cli_network       *network;
	struct cli_io            *io;
	struct cli_console       *console;
	struct cli_state         *saved;
	struct autocomplete_file *files;
	struct json_object       *history;
	struct cli_options const *options;
	struct cli_console_lines  inputs;
	struct list               tools;
	uint64_t                  exit_due;
	int                       ui_error;
	uint16_t                  exit_signal;
	uint8_t                   flags;
	_Atomic bool              cancel_io;
};

/** The flags of struct cli. */
enum {
	CLI_BUSY     = 1u << 0,  /**< the chat waits for a turn or blocking work */
	CLI_QUITTING = 1u << 1,  /**< the chat is ending */
};

/** Returns true once a signal or the end of the chat stops new work. */
static bool
cli_stopping (struct cli const *cli)
{
	return cli->exit_signal || (cli->flags & CLI_QUITTING);
}

enum { CLI_INPUT_HANDLED = 1u << 0 };

static int cli_immediate (struct cli *cli);

/* Store @p e as the error of the UI, unless @p e is 0 or the UI already has
 * an error.
 */
static void
cli_fail (struct cli *cli,
          int         e)
{
	if (e && !cli->ui_error)
		cli->ui_error = e;
}

static void
cli_shutdown (struct cli *cli)
{
	if (!cli->exit_due)
		cli->exit_due = loop_clock() + 2 * NS;
	cli->reactor.until = cli->exit_due;
	atomic_store(&cli->cancel_io, true);
	cli_network_cancel(cli->network, true);
	if (cli->console)
		cli_console_interrupt(cli->console, true);
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
			cli_console_resize(cli->console);
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
		/* In a chat on a terminal, it cancels the work under way, or the
		 * draft. Otherwise it stops the client. */
		if ((cli->options->flags & (CLI_TTY | CLI_SINGLE)) != CLI_TTY)
			goto stop;
		if (cli->flags & CLI_BUSY)
			atomic_store(&cli->cancel_io, true);
		else if (cli_network_active(cli->network))
			cli_tool_cancel_last(&cli->tools);
		else if (cli->options->flags & CLI_SIMPLE)
			goto stop;
		else if (cli->console)
			cli_console_interrupt(cli->console, false);
		cli_network_cancel(cli->network, false);
		break;
	case SIGTERM:
	case SIGHUP:
	stop:
		if (!cli->exit_signal || cli->exit_signal == SIGINT)
			cli->exit_signal = number;
		cli_shutdown(cli);
		break;
	}
	cli_fail(cli, e);
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
	_Exit(cli->exit_signal ? 128 + cli->exit_signal : (cli->flags & CLI_QUITTING) && !cli->ui_error ? 0 : 1);
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
		if (!e && !(cli->flags & CLI_QUITTING))
			e = cli_immediate(cli);
		if (e != EINTR)
			cli_fail(cli, e);
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
	if (cli->flags & CLI_QUITTING)
		return 0;
	int e = cli_ui_error(cli->console, title, detail);
	cli_fail(cli, e);
	return e;
}

/** A job that the chat waits for. It is embedded in the struct of the
 *  arguments of the job, and the work function of the job computes the
 *  address of that struct with container_of().
 */
struct cli_work {
	struct cli_io_job job;
	uint64_t          done;  /**< Nonzero after cli_work_done() runs. It is
	                              a uint64_t to fill the tail padding. */
};

static void
cli_work_done (struct cli_io_job *job)
{
	struct cli_work *work = container_of(job, struct cli_work, job);
	work->done = true;
}

/* Run work, whose job.work is set, on a blocker, and turn the loop of the
 * UI until it is done. Returns the error of the work, or of its submission.
 */
static int
cli_work_wait (struct cli      *cli,
               struct cli_work *work)
{
	if (cli_stopping(cli))
		return EINTR;
	work->job.done = cli_work_done;
	int e = cli_io_submit(cli->io, &work->job);
	if (e)
		return e;
	uint8_t busy = cli->flags & CLI_BUSY;
	cli->flags |= CLI_BUSY;
	while (!work->done)
		cli_wait(cli, cli->exit_due ? cli->exit_due : UINT64_MAX);
	cli->flags = (cli->flags & ~CLI_BUSY) | busy;
	if (cli_stopping(cli))
		return EINTR;
	if (!busy)
		atomic_store(&cli->cancel_io, false);
	return work->job.blocker.error;
}

enum cli_history_operation : uint64_t {
	CLI_HISTORY_OPEN,
	CLI_HISTORY_INPUT,
	CLI_HISTORY_TURN,
	CLI_HISTORY_REPLACE,  /**< replace the last turn */
	CLI_HISTORY_CLEAR,
};

struct cli_history_work {
	struct cli_work             work;
	struct cli_state           *state;
	struct cstr_view            path;
	struct cstr_view            text;
	struct cstr_view            answer;
	_Atomic bool const         *cancel;
	enum cli_history_operation  operation;
};

static int
cli_history_work (struct cli_io_job *job)
{
	struct cli_history_work *work = container_of(job, struct cli_history_work, work.job);
	switch (work->operation) {
	case CLI_HISTORY_OPEN: {
		int e;
		work->state = cli_state_create(work->path, work->text, work->cancel, &e);
		return e;
	}
	case CLI_HISTORY_INPUT:
		return cli_state_add_input(work->state, work->text);
	case CLI_HISTORY_TURN:
	case CLI_HISTORY_REPLACE:
		return cli_state_add_turn(work->state, work->text, work->answer, work->operation == CLI_HISTORY_REPLACE);
	case CLI_HISTORY_CLEAR:
		return cli_state_clear_turns(work->state);
	}
	return EINVAL;
}

static int
cli_history_update (struct cli                  *cli,
                    enum cli_history_operation  operation,
                    struct cstr_view            text,
                    struct cstr_view            answer)
{
	struct cli_history_work work = {.work.job.work = cli_history_work, .state = cli->saved,
	                                .operation = operation, .text = text, .answer = answer};
	return cli_work_wait(cli, &work.work);
}

struct cli_file_work {
	struct cli_work                 work;
	struct autocomplete_file const *files;
	struct dstr                    *text;
	struct cstr_view                path;
	struct cli_error               *error;
};

static int
cli_file_work (struct cli_io_job *job)
{
	struct cli_file_work *work = container_of(job, struct cli_file_work, work.job);
	return cli_read_file(work->files, work->text, work->path, work->error);
}

static int
cli_file_read (struct cli       *cli,
               struct dstr      *text,
               struct cstr_view  path,
               struct cli_error *error)
{
	struct cli_file_work work = {.work.job.work = cli_file_work, .files = cli->files, .text = text,
	                             .path = path, .error = error};
	return cli_work_wait(cli, &work.work);
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
	return cli_save_error(cli, cli_history_update(cli, CLI_HISTORY_INPUT, input, (struct cstr_view){}), error);
}

/** Mark the client busy or idle; manual tool calls keep it shown busy. */
static int
cli_busy (struct cli *cli,
          bool        busy)
{
	cli->flags = (cli->flags & ~CLI_BUSY) | (busy ? CLI_BUSY : 0);
	return cli_console_busy(cli->console, busy || !list_is_empty(&cli->tools));
}

struct cli_completion_work {
	struct cli_work                  work;
	struct autocomplete_file        *files;
	struct cstr_view                 line;
	struct autocomplete_match const *matches;
	size_t                           cursor;
	size_t                           count;
};

static int
cli_completion_work (struct cli_io_job *job)
{
	struct cli_completion_work *work = container_of(job, struct cli_completion_work, work.job);
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
		struct cli_completion_work work = {.work.job.work = cli_completion_work, .files = cli->files,
		                                   .line = line, .cursor = cursor};
		e = cli_work_wait(cli, &work.work);
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
	struct cli_work  work;
	char const      *path;
	struct dstr     *identity;
};

static int
cli_path_work (struct cli_io_job *job)
{
	struct cli_path_work *work = container_of(job, struct cli_path_work, work.job);
	char *path = realpath(work->path, nullptr);
	if (!path)
		return errno;
	int e = dstr_cat(work->identity, "socket=") &&
	        dstr_cat(work->identity, path) ? 0 : ENOMEM;
	free(path);
	return e;
}

static int
cli_history_open (struct cli       *cli,
                  struct cli_error *error)
{
	struct cli_options const *options = cli->options;
	if ((options->flags & CLI_NO_HISTORY) ||
	    (!(options->flags & CLI_TTY) && (!options->history_repo || !*options->history_repo)))
		return 0;
	dstr_auto identity = dstr(nullptr);
	struct cstr_view key = options->session_id ? cstr_view(options->session_id)
	                                           : cstr_view_dstr(&cli->client.session_key);
	if (key.len) {
		if (!dstr_cat(&identity, "session=") || cstr_view_cat(key, &identity))
			return ENOMEM;
	} else {
		struct cli_path_work work = {.work.job.work = cli_path_work, .path = options->socket,
		                             .identity = &identity};
		int e = cli_work_wait(cli, &work.work);
		if (e)
			return cli_error_set(error, e, "Cannot identify history connection: %s", strerror(e));
	}
	/* The value of the key participant is the agent. The key is still
	 * named participant, so --session-id with the session of an older
	 * askai opens the history of its participant of the same name. */
	struct cstr_view const parts[] = {
		CSTR_VIEW("\0participant="), cstr_view_dstr(&cli->client.agent),
		CSTR_VIEW("\0tool="), cstr_view_dstr(&cli->client.tool),
		CSTR_VIEW("\0system-policy="), options->system ? CSTR_VIEW("override") : CSTR_VIEW("default"),
		CSTR_VIEW("\0system="), cstr_view(options->system)
	};
	for (size_t i = 0; i < array_size(parts); ++i)
		if (cstr_view_cat(parts[i], &identity))
			return ENOMEM;
	struct cli_history_work work = {.work.job.work = cli_history_work, .operation = CLI_HISTORY_OPEN,
	                                .cancel = &cli->cancel_io, .path = cstr_view(options->history_repo),
	                                .text = cstr_view_dstr(&identity)};
	int e = cli_work_wait(cli, &work.work);
	cli->saved = work.state;
	if (e)
		return cli_error_set(error, e, "Cannot open client history repository: %s", strerror(e));
	for (size_t i = 0, n = cli_state_input_count(cli->saved); !e && i < n; ++i)
		e = cli_console_remember(cli->console, cli_state_input_at(cli->saved, i));
	for (size_t i = 0, n = cli_state_turn_count(cli->saved); !e && i < n; ++i) {
		struct cli_state_turn const *turn = cli_state_turn_at(cli->saved, i);
		e = jb_push(cli->history, jb_object("prompt", cli_client_string(cstr_view_dstr(&turn->prompt)),
		                                   "answer", cli_client_string(cstr_view_dstr(&turn->answer)), nullptr));
	}
	size_t turns = json_object_array_length(cli->history);
	if (e || !(options->flags & CLI_TTY) || !turns)
		return e;
	char text[96];
	return cli_ui_info(cli->console, util_format_view(text, sizeof text, "Restored %zu completed turns. "
	                                                  "/history displays them; /clear starts over.", turns));
}

static int
cli_ask (struct cli          *cli,
         struct cstr_view     prompt,
         size_t               turns,
         struct json_object **answer,
         struct cli_error    *error)
{
	*answer = nullptr;
	cli->client.flags |= dstr_len(&cli->client.notes_tool) ? CLI_CLIENT_NOTES_DIRTY : 0;
	if (turns >= CLI_STATE_TURNS)
		return cli_error_set(error, E2BIG, "Conversation has %d turns; use /clear", CLI_STATE_TURNS);
	if (prompt.len > CLI_LIMIT)
		return cli_error_set(error, EFBIG, "Conversation exceeds the client limit; use /clear");
	size_t bytes = prompt.len;
	cli_json_auto messages = json_object_new_array();
	if (!messages)
		return ENOMEM;
	/* Each turn is a message of the user and one of the assistant. */
	static char const *const keys[] = {"prompt", "answer"};
	static char const *const roles[] = {"user", "assistant"};
	for (size_t k = 0; k < 2 * turns; ++k) {
		struct json_object *value = jb_get(jb_at(cli->history, k / 2), keys[k % 2]);
		struct cstr_view text = jb_view(value);
		if (text.len > CLI_LIMIT - bytes)
			return cli_error_set(error, EFBIG, "Conversation exceeds the client limit; use /clear");
		bytes += text.len;
		int e = jb_push(messages, jb_object("role", json_object_new_string(roles[k % 2]),
		                                    "content", json_object_get(value), nullptr));
		if (e)
			return e;
	}
	cli_json_auto args = jb_object("prompt", cli_client_string(prompt),
	                               "background", json_object_new_boolean(false),
	                               "messages", json_object_get(messages), nullptr);
	if (!args)
		return ENOMEM;
	if (cli->options->system && jb_set(args, "system", cli_client_string(cstr_view(cli->options->system))))
		return ENOMEM;
	cli_json_auto params = jb_object("name", cli_client_string(cstr_view_dstr(&cli->client.tool)),
	                                 "arguments", json_object_get(args),
	                                 "_meta", jb_object("askai", jb_object("stream", json_object_new_boolean(true), nullptr),
	                                                    "progressToken", json_object_new_string("cli"), nullptr), nullptr);
	if (!params)
		return ENOMEM;
	uint64_t flags = CLI_STREAM_DELTAS | (cli->options->flags & CLI_NO_REASONING ? 0 : CLI_STREAM_REASONING);
	struct cli_stream stream = cli_stream(cli->console, cstr_view_dstr(&cli->client.tool),
	                                      CSTR_VIEW("cli"), flags);
	cli_json_auto result = nullptr;
	int e = cli_client_rpc(&cli->client, CLI_TOOLS_CALL, json_object_get(params),
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

	for (size_t i = 0, count = autocomplete_tool_count(cli->client.catalog); i < count; ++i) {
		struct autocomplete_tool const *entry = autocomplete_tool_at(cli->client.catalog, i);
		struct cstr_view summary = entry->description;
		summary.len = jb_utf8(summary.data, summary.len < 160 ? summary.len : 160);
		(void)cstr_view_cut(&summary, '\n', &summary);
		struct cli_console_span const spans[] = {
			{entry->name, CLI_STYLE_RESET},
			{summary.len ? CSTR_VIEW("  ") : (struct cstr_view){}, CLI_STYLE_RESET},
			{summary, CLI_STYLE_RESET}, {CSTR_VIEW("\n"), CLI_STYLE_RESET},
		};
		int e = cli_console_print(cli->console, spans, array_size(spans));
		if (e)
			return e;
	}
	return 0;
}

static void
cli_manual_done (struct cli_tool        *tool,
                 void                   *context,
                 int                     e,
                 struct json_object     *result,
                 struct cli_error const *error)
{
	struct cli *cli = context;
	if (!cli_stopping(cli)) {
		cli_fail(cli, cli_tool_finish(tool, e, result, error));
		if (cli->client.flags & (CLI_CLIENT_CATALOG_DIRTY | CLI_CLIENT_NOTES_DIRTY))
			cli_console_yield(cli->console);
	}
	cli_fail(cli, cli_busy(cli, cli->flags & CLI_BUSY));
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
	if ((cli->options->flags & (CLI_TTY | CLI_SIMPLE | CLI_SINGLE)) == CLI_TTY) {
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
	struct cstr_view const tool = cstr_view_dstr(&cli->client.tool);
	for (size_t i = 0, count = json_object_array_length(cli->history); i < count; ++i) {
		struct json_object *turn = json_object_array_get_idx(cli->history, i);
		struct cli_console_span const spans[] = {
			{CSTR_VIEW("you:\n"), CLI_STYLE_INFO},
			{jb_view(jb_get(turn, "prompt")), CLI_STYLE_RESET}, {CSTR_VIEW("\n"), CLI_STYLE_RESET},
			{tool, CLI_STYLE_INFO}, {CSTR_VIEW(":\n"), CLI_STYLE_INFO},
			{jb_view(jb_get(turn, "answer")), CLI_STYLE_RESET}, {CSTR_VIEW("\n"), CLI_STYLE_RESET},
		};
		int e = cli_console_print(cli->console, spans, array_size(spans));
		if (e)
			return e;
	}
	return 0;
}

/* Print the members of the session, such as "People: imaami. Agents:
 * bro, cuh.", from the member list and the agent list of the catalog.
 */
static int
cli_names (struct cli *cli)
{
	struct autocomplete const *catalog = cli->client.catalog;
	size_t const count = autocomplete_list_count(catalog, AUTOCOMPLETE_MEMBERS);
	if (!count)
		return cli_ui_message(cli->console, CSTR_VIEW("No members discovered. /refresh reloads the members of the session."));
	size_t const agents = autocomplete_list_count(catalog, AUTOCOMPLETE_AGENTS);
	dstr_auto names = dstr(nullptr);
	size_t people = 0;
	/* Both lists are sorted. A member that is not in the agent list is a
	 * person. */
	for (size_t i = 0, a = 0; i < count; ++i) {
		struct cstr_view const name = autocomplete_list_at(catalog, AUTOCOMPLETE_MEMBERS, i);
		while (a < agents && cstr_view_compare(autocomplete_list_at(catalog, AUTOCOMPLETE_AGENTS, a), name) < 0)
			++a;
		if (cstr_view_equal(autocomplete_list_at(catalog, AUTOCOMPLETE_AGENTS, a), name))
			continue;
		if (!dstr_cat(&names, people++ ? ", " : "People: ") || cstr_view_cat(name, &names))
			return ENOMEM;
	}
	for (size_t a = 0; a < agents; ++a) {
		struct cstr_view const name = autocomplete_list_at(catalog, AUTOCOMPLETE_AGENTS, a);
		if (!dstr_cat(&names, a ? ", " : people ? ". Agents: " : "Agents: ") || cstr_view_cat(name, &names))
			return ENOMEM;
	}
	if (!dstr_cat(&names, "."))
		return ENOMEM;
	return cli_ui_message(cli->console, cstr_view_dstr(&names));
}

enum cli_action { CLI_ACTION_PROMPT, CLI_ACTION_CONTINUE, CLI_ACTION_EXIT, CLI_ACTION_REGENERATE };

static struct cli_command const *
cli_command_parse (struct cstr_view  line,
                   struct cstr_view *rest)
{
	line = cstr_view_trim(line);
	size_t split = 0;
	while (split < line.len && !cstr_view_space(line.data[split]))
		++split;
	*rest = cstr_view_trim(cstr_view_slice(line, split, SIZE_MAX));
	struct cli_command const *command = cli_command_find(cstr_view_slice(line, 0, split));
	return command && (command->args != CLI_COMMAND_ARGS_NONE || !rest->len) ? command : nullptr;
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
		return cli_ui_message(cli->console, cli_commands_text);
	case CLI_COMMAND_TOOLS:
		return cli_tools(cli, rest, error);
	case CLI_COMMAND_NAMES:
		return cli_names(cli);
	case CLI_COMMAND_REFRESH: {
		int e = cli_client_refresh(&cli->client, error);
		return e ? e : cli_ui_info(cli->console, CSTR_VIEW("Tools and session members refreshed."));
	}
	case CLI_COMMAND_TOOL:
		if (cli->options->flags & CLI_SINGLE)
			*action = CLI_ACTION_EXIT;
		/* autocomplete_parse_call() skips the white space around the words. */
		return cli_manual_call(cli, cstr_view_dstr(prompt), error);
	case CLI_COMMAND_HISTORY:
		return cli_history_show(cli);
	case CLI_COMMAND_CLEAR: {
		size_t count = json_object_array_length(cli->history);
		if (count)
			(void)json_object_array_del_idx(cli->history, 0, count);
		int e = cli->saved ? cli_save_error(cli, cli_history_update(cli, CLI_HISTORY_CLEAR, (struct cstr_view){}, (struct cstr_view){}), error) : 0;
		return e || !(cli->options->flags & CLI_TTY) ? e : cli_ui_info(cli->console, CSTR_VIEW("Conversation cleared."));
	}
	case CLI_COMMAND_REGEN: {
		size_t count = json_object_array_length(cli->history);
		if (!count)
			return cli_error_set(error, EINVAL, "There is no previous answer to regenerate");
		struct cstr_view previous = jb_view(jb_get(jb_at(cli->history, count - 1), "prompt"));
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
	if (cli->flags & CLI_QUITTING)
		return false;
	struct cstr_view rest;
	struct cli_command const *command = cli_command_parse(line, &rest);
	return command && (command->id == CLI_COMMAND_QUIT || command->id == CLI_COMMAND_EXIT ||
	                   ((cli->flags & CLI_BUSY) && command->policy != CLI_COMMAND_WAIT));
}

static int
cli_input_append (struct cli  *cli,
                  struct dstr *text,
                  size_t       flags)
{
	if (dstr_len(text) > CLI_LIMIT - cli->inputs.used.bytes)
		return ENOBUFS;
	return cli_console_lines_push(&cli->inputs, text, flags);
}

/* Save a handled input in the history. If a cancel interrupted the save,
 * queue the input again. Returns 0 or an error number.
 */
static int
cli_input_save (struct cli       *cli,
                struct dstr      *text,
                struct cli_error *error)
{
	int e = cli_history_input(cli, cstr_view_dstr(text), true, error);
	if (e != EINTR || cli_stopping(cli))
		return e;
	atomic_store(&cli->cancel_io, false);
	return cli_input_append(cli, text, CLI_INPUT_HANDLED);
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
		cli->flags |= CLI_QUITTING;
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
	struct cstr_view text = cstr_view_dstr(prompt);
	struct cstr_view rest = {};
	struct cli_command const *command = interpret ? cli_command_parse(text, &rest) : nullptr;
	if (command && (command->policy == CLI_COMMAND_LOCAL ||
	                (command->policy == CLI_COMMAND_CONCURRENT &&
	                 (cli->options->flags & (CLI_TTY | CLI_SIMPLE | CLI_SINGLE)) == CLI_TTY))) {
		int e = cli_command_run(cli, prompt, command, rest, action, error);
		if (!e && cli->saved && *action != CLI_ACTION_EXIT)
			e = cli_input_append(cli, prompt, CLI_INPUT_HANDLED);
		return e;
	}
	if (cli->saved) {
		int e = cli_history_input(cli, text, interpret, error);
		if (e)
			return e;
	}
	*action = CLI_ACTION_PROMPT;
	if (command) {
		int e = cli_command_run(cli, prompt, command, rest, action, error);
		if (e || *action == CLI_ACTION_CONTINUE || *action == CLI_ACTION_EXIT)
			return e;
		/* /regen and /read replace the text of the prompt. */
		text = cstr_view_dstr(prompt);
	}
	if (atomic_load(&cli->cancel_io))
		return EINTR;
	if (!cstr_view_trim(text).len) {
		*action = CLI_ACTION_CONTINUE;
		return cli->options->flags & CLI_SINGLE ? cli_error_set(error, EINVAL, "The prompt is empty") : 0;
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
		return ENOMEM;
	}
	if (cli->saved)
		e = cli_save_error(cli, cli_history_update(cli, regenerate ? CLI_HISTORY_REPLACE : CLI_HISTORY_TURN,
		                                           text, jb_view(answer)), error);
	if (cli->options->flags & CLI_SINGLE)
		*action = CLI_ACTION_EXIT;
	return e;
}

/* Refresh the completion metadata or the note filenames if one of them is
 * out of date and no input or tool call is queued. Report a failed refresh
 * in the console. Returns 0, or the error number that ends the chat.
 */
static int
cli_chat_refresh (struct cli       *cli,
                  struct cli_error *error)
{
	uint64_t dirty = cli->client.flags & (CLI_CLIENT_CATALOG_DIRTY | CLI_CLIENT_NOTES_DIRTY);
	if (!dirty || cli_console_queued(cli->console) || !list_is_empty(&cli->tools))
		return 0;
	bool catalog = dirty & CLI_CLIENT_CATALOG_DIRTY;
	int e = cli_busy(cli, true);
	if (!e)
		e = catalog ? cli_client_refresh(&cli->client, error)
		            : cli_client_refresh_notes(&cli->client, error);
	int display = cli_busy(cli, false);
	if (!e)
		e = display;
	switch (e) {
	case EINTR:
		if (cli_stopping(cli) || (cli->options->flags & CLI_SINGLE))
			return e;
		atomic_store(&cli->cancel_io, false);
		return 0;
	case 0:
	case ENOMEM:
		return e;
	}
	if ((cli->flags & CLI_QUITTING) || (cli->client.flags & CLI_CLIENT_IDENTITY_CHANGED))
		return e;
	e = cli_ui_error(cli->console, catalog ? CSTR_VIEW("Cannot refresh completion metadata")
	                                       : CSTR_VIEW("Cannot refresh note filenames"),
	                 cli_error_text(error, e));
	if (!e)
		util_text_clear(error);
	return e;
}

/* Report the error e of a turn in the console, unless it ends the chat.
 * Report EINTR with the message "Cancelled." Returns 0, or the error number
 * that ends the chat.
 */
static int
cli_chat_error (struct cli       *cli,
                int               e,
                struct cli_error *error)
{
	uint32_t const flags = cli->options->flags;
	if (flags & CLI_SINGLE)
		return e;
	switch (e) {
	case EINTR:
		if (cli->exit_signal)
			return e;
		atomic_store(&cli->cancel_io, false);
		return cli_ui_info(cli->console, CSTR_VIEW("Cancelled."));
	case ENOMEM:
		return e;
	}
	if (!(flags & CLI_TTY) || ferror(stdout))
		return e;
	return cli_ui_error(cli->console, cli_error_text(error, e), (struct cstr_view){});
}

/* Move the first queued input into prompt. An input that a command
 * handled already is only saved in the history. Returns 0 to run a turn
 * with prompt, EAGAIN to look for the next input, or an error number.
 */
static int
cli_chat_pop (struct cli       *cli,
              struct dstr      *prompt,
              struct cli_error *error)
{
	if (!(cli_console_lines_pop(&cli->inputs, prompt) & CLI_INPUT_HANDLED))
		return 0;
	int e = cli_input_save(cli, prompt, error);
	return e ? e : EAGAIN;
}

/* Read the next prompt into prompt while no input is queued. First refresh
 * what is out of date, then use the initial prompt, which is not
 * interpreted as a command, or read the console. At the end of the input,
 * set CLI_QUITTING. Returns 0 to run a turn with prompt, EAGAIN to look
 * for the next input, or an error number.
 */
static int
cli_chat_read (struct cli       *cli,
               struct dstr      *prompt,
               bool             *initial,
               bool             *interpret,
               struct cli_error *error)
{
	struct cli_options const *options = cli->options;
	int e = cli_chat_refresh(cli, error);
	if (e)
		return e;
	if ((cli->flags & CLI_QUITTING) || cli->inputs.used.n)
		return EAGAIN;
	if (*initial) {
		*interpret = false;
		e = options->file ? cli_file_read(cli, prompt, cstr_view(options->file), error)
		  : dstr_cat(prompt, options->prompt) ? 0 : ENOMEM;
		*initial = e != 0;
		return e;
	}
	bool eof;
	e = cli_console_read(cli->console, prompt, options->flags & CLI_MULTILINE, &eof,
	                     options->flags & CLI_TTY ? CSTR_VIEW("\n> ") : (struct cstr_view){});
	if (!e && eof) {
		cli->flags |= CLI_QUITTING;
		return EAGAIN;
	}
	return e;
}

static int
cli_chat (struct cli       *cli,
          struct cli_error *error)
{
	bool initial = cli->options->prompt || cli->options->file;
	for (;;) {
		if (cli->flags & CLI_QUITTING)
			return 0;
		if (cli->ui_error)
			return cli->ui_error;
		if (cli->exit_signal)
			return EINTR;
		util_text_clear(error);
		dstr_auto prompt = dstr(nullptr);
		bool interpret = true;
		int e = cli->inputs.used.n ? cli_chat_pop(cli, &prompt, error)
		                          : cli_chat_read(cli, &prompt, &initial, &interpret, error);
		if (e == EAGAIN)
			continue;
		if (e)
			return cli->flags & CLI_QUITTING ? 0 : e;
		enum cli_action action;
		e = cli_busy(cli, true);
		if (!e)
			e = cli_turn(cli, &prompt, interpret, &action, error);
		int display = cli_busy(cli, false);
		if (!e)
			e = display;
		if (cli->flags & CLI_QUITTING)
			return 0;
		if ((cli->client.flags & CLI_CLIENT_IDENTITY_CHANGED) || (!e && action == CLI_ACTION_EXIT))
			return e;
		e = e ? cli_chat_error(cli, e, error) : 0;
		if (e)
			return e;
	}
}

/* Write the output that the console queued, and run turns until it is
 * written, a signal arrives or an error occurs. Returns 0 or an error
 * number.
 */
static int
cli_drain (struct cli *cli)
{
	int e = cli_console_flush(cli->console);
	while (!e && cli_console_pending(cli->console) && !cli->exit_signal) {
		e = cli_loop_turn(&cli->reactor, UINT64_MAX);
		if (!e)
			e = cli->ui_error;
	}
	return e;
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
	list_init(&cli->inputs.lines);
	list_init(&cli->tools);
	cli->options = options;
	cli->signals.ref.fd = -1;
	loop_place(&cli->reactor.loop);
	int e = cli_loop_init(&cli->reactor);
	if (!e)
		e = cli_signals_init(&cli->signals, &cli->reactor.loop, cli_signal);
	bool curl_ready = false;
	if (!e) {
		e = curl_global_init(CURL_GLOBAL_DEFAULT) ? EIO : 0;
		curl_ready = !e;
	}
	if (!e)
		cli->network = cli_network_create(&cli->reactor, &e);
	if (!e)
		cli->io = cli_io_create(&cli->reactor.loop, &e);
	if (!e)
		cli->client = cli_client(options->socket, options->tool, cli->network, &e);
	bool color = options->color == CLI_COLOR_ON ||
	             (options->color == CLI_COLOR_AUTO && (options->flags & CLI_TTY) && !getenv("NO_COLOR"));
	if (!e)
		cli->console = cli_console_create((options->flags & CLI_SIMPLE ? CLI_CONSOLE_SIMPLE : 0) |
		                                  (color ? CLI_CONSOLE_COLOR : 0), &cli->reactor, cli->io, &e);
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
	if (!e && (options->flags & CLI_TTY)) {
		struct cli_console_span const spans[] = {
			{CSTR_VIEW("Connected to "), CLI_STYLE_INFO},
			{cstr_view_dstr(&cli->client.tool), CLI_STYLE_INFO},
			{CSTR_VIEW(". /help lists commands; Ctrl-C cancels an answer.\n"), CLI_STYLE_INFO},
		};
		e = cli_console_print(cli->console, spans, array_size(spans));
		if (!e && (cli->client.flags & CLI_CLIENT_OLDER))
			e = cli_ui_info(cli->console, CSTR_VIEW("This askai is older than askai-cli. Notebook completion is off."));
	}
	if (!e)
		e = cli_history_open(cli, error);
	if (!e)
		cli_console_set_immediate(cli->console, cli_immediate_command, cli);
	if (!e)
		e = cli_chat(cli, error);
	cli->flags |= CLI_QUITTING;
	if (!list_is_empty(&cli->tools)) {
		cli_shutdown(cli);
		while (!list_is_empty(&cli->tools))
			cli_wait(cli, cli->exit_due);
	}
	if (!e && cli->ui_error)
		e = cli->ui_error;
	if (cli->exit_signal)
		e = EINTR;
	if (!e && cli->console)
		e = cli_drain(cli);
	/* End editor activity before stopping I/O, retaining console storage
	 * until any outstanding regular-file operation has returned. */
	if (cli->console)
		(void)cli_console_suspend(cli->console);
	cli_io_stop(cli->io);
	uint64_t due = cli->exit_due ? cli->exit_due : loop_clock() + 2 * NS;
	while (!cli_io_done(cli->io))
		cli_wait(cli, due);
	cli_network_stop(cli->network);
	while (!cli_network_done(cli->network))
		cli_wait(cli, due);
	cli_network_destroy(&cli->network);
	cli_console_destroy(&cli->console);
	cli_io_destroy(&cli->io);
	cli_state_destroy(&cli->saved);
	cli_console_lines_fini(&cli->inputs);
	autocomplete_file_destroy(&cli->files);
	cli_client_json_cleanup(&cli->history);
	cli_client_fini(&cli->client);
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
	return e;
}

int
main (int    argc,
      char **argv)
{
	struct cli_options options = {};
	struct cli_error error = {};
	unsigned exit_signal = 0;
	/* Before askai-cli opens any file, so that none takes such a number. */
	int e = child_std_open();
	if (e)
		(void)cli_error_set(&error, e, "Cannot open /dev/null: %s", strerror(e));
	else
		e = cli_args(&options, argc, argv, &error);
	if (!e && !(options.flags & CLI_DONE))
		e = cli_run(&options, &error, &exit_signal);
	if (e == EINTR && exit_signal)
		return 128 + (int)exit_signal;
	if (e) {
		struct cstr_view const line[] = {CSTR_VIEW("askai-cli: "), cli_error_text(&error, e), CSTR_VIEW("\n")};
		(void)cli_console_write_error(line, array_size(line));
	}
	return e ? 1 : 0;
}
