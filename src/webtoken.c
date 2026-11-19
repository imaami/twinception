/** @file webtoken.c
 *
 * askai-open-browser URL prints the JSON Web Token of the page URL, which
 * serves the token to a user who is signed in, as a single sign-on page
 * does. It is a command for --auth-command of askai (src/bearer.h), and
 * runs in WSL2 with the browser of Windows, or on Linux.
 *
 * The page cannot hand its content to another program, so the program
 * tries these steps, each one only if the one before gave no token:
 *
 * 1. curl.exe of Windows fetches the page with the integrated
 *    authentication of the user who is logged on to Windows. No browser
 *    opens. This works only if the sign-on accepts Kerberos or NTLM from
 *    the host of the URL itself: curl does not authenticate to a host that
 *    the page redirects to.
 * 2. The default browser opens the page: the browser of BROWSER, of Windows
 *    in WSL2, or of xdg-open. The user copies the text of the page (Ctrl+A,
 *    Ctrl+C), and the program takes the token from the clipboard of
 *    Windows, which powershell.exe reads, and empties the clipboard.
 * 3. The user pastes the token at the terminal.
 *
 * The program writes its messages to the terminal, /dev/tty. Without one,
 * as under a daemon, step 3 is left out, and the open page is the only
 * sign that the program waits for the clipboard.
 */
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "buf.h"
#include "child.h"
#include "clk.h"
#include "compat.h"
#include "cstr_view.h"
#include "jwt.h"
#include "loop.h"
#include "util.h"

enum {
	WEBTOKEN_OUT   = 4 * JWT_LIMIT,  /* the bytes read from a program */
	WEBTOKEN_FETCH = 20,             /* the seconds of step 1 */
	WEBTOKEN_READ  = 10,             /* the seconds of a read of the clipboard */
	WEBTOKEN_COPY  = 120,            /* the seconds of the wait of step 2 */
};

/* The programs of Windows that the steps run. */
enum webtoken_exe {
	WEBTOKEN_CURL,
	WEBTOKEN_POWERSHELL,
	WEBTOKEN_CLIP,
	WEBTOKEN_RUNDLL,
};

/* Where WSL mounts each program, for a PATH without the directories of
 * Windows.
 */
static char const *const webtoken_exes[] = {
	[WEBTOKEN_CURL]       = "/mnt/c/Windows/System32/curl.exe",
	[WEBTOKEN_POWERSHELL] = "/mnt/c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe",
	[WEBTOKEN_CLIP]       = "/mnt/c/Windows/System32/clip.exe",
	[WEBTOKEN_RUNDLL]     = "/mnt/c/Windows/System32/rundll32.exe",
};

extern char **environ;

/* Runs the program @p exe of Windows with the words @p argv after its
 * name, up to @p seconds, and replaces the content of @p out with its
 * output. The program is the one of PATH, argv[0], or else the one that
 * WSL mounts. Returns what child_cmd_run() returns.
 */
static int
webtoken_run (enum webtoken_exe   exe,
              char const        **argv,
              unsigned            seconds,
              struct buf         *out)
{
	struct child_cmd cmd = child_cmd(
		.file     = argv[0],
		.argv     = argv,
		.envp     = environ,
		.max      = WEBTOKEN_OUT,
		.deadline = loop_clock() + seconds * NS,
	);
	struct child_end end;
	int e = child_cmd_run(&cmd, child_fds(), out, &end);
	if (e != ENOENT)
		return e;

	cmd.file = argv[0] = webtoken_exes[exe];
	return child_cmd_run(&cmd, child_fds(), out, &end);
}

/* Step 1: writes the token of the page @p url to @p *token, a view of
 * @p out, or an empty view if curl.exe gets none.
 */
static void
webtoken_fetch (char const       *url,
                struct buf       *out,
                struct cstr_view *token)
{
	char const *argv[] = {
		"curl.exe", "--silent", "--fail", "--location", "--negotiate", "--user", ":",
		"--cookie", "", "--max-time", "15", url, nullptr,
	};
	*token = webtoken_run(WEBTOKEN_CURL, argv, WEBTOKEN_FETCH, out)
	         ? cstr_view(nullptr) : jwt_find(cstr_view_span(buf_peek(out)));
}

/* Replaces the content of @p out with the text of the clipboard of
 * Windows. Returns what webtoken_run() returns.
 */
static int
webtoken_clipboard (struct buf *out)
{
	char const *argv[] = {
		"powershell.exe", "-NoProfile", "-NonInteractive", "-Command",
		"Get-Clipboard -Raw", nullptr,
	};
	return webtoken_run(WEBTOKEN_POWERSHELL, argv, WEBTOKEN_READ, out);
}

/* Opens @p url in the browser of BROWSER, of Windows in WSL2, or of
 * xdg-open, without a wait for the browser, whose output is /dev/null.
 * Returns 0 or the error of the spawn.
 */
static int
webtoken_open (char const *url)
{
	char const *browser = getenv("BROWSER");
	char const *wsl = getenv("WSL_DISTRO_NAME");
	bool const windows = (!browser || !*browser) && wsl && *wsl;
	char *argv[] = {
		(char *)(windows ? "rundll32.exe" : browser && *browser ? browser : "xdg-open"),
		(char *)(windows ? "url.dll,FileProtocolHandler" : url),
		(char *)(windows ? url : nullptr),
		nullptr,
	};
	posix_spawn_file_actions_t actions;
	int e = posix_spawn_file_actions_init(&actions);
	if (e)
		return e;

	for (int fd = 0; !e && fd < 3; ++fd)
		e = posix_spawn_file_actions_addopen(&actions, fd, "/dev/null", O_RDWR, 0);
	pid_t pid;
	if (!e)
		e = posix_spawnp(&pid, argv[0], &actions, nullptr, argv, environ);
	if (e == ENOENT && windows)
		e = posix_spawn(&pid, webtoken_exes[WEBTOKEN_RUNDLL], &actions, nullptr, argv,
		                environ);
	(void)posix_spawn_file_actions_destroy(&actions);
	return e;
}

/* Writes @p text to the terminal @p tty, if there is one. */
#define webtoken_say(tty, text) \
        ((tty) < 0 ? (void)0 : (void)util_write_all((tty), text, sizeof text - 1))

/* Step 2: opens the page @p url, and waits until the clipboard of Windows
 * contains a token that it did not contain before. Writes the token to
 * @p *token, a view of @p out, or an empty view if the browser or the
 * clipboard cannot be used, or after WEBTOKEN_COPY seconds. The clipboard
 * is emptied after it gave the token.
 */
static void
webtoken_copy (char const       *url,
               int               tty,
               struct buf       *out,
               struct cstr_view *token)
{
	*token = cstr_view(nullptr);
	/* The token that the clipboard contains now is an old one. */
	struct buf before = buf();
	bool const readable = !webtoken_clipboard(&before);
	struct cstr_view const old = jwt_find(cstr_view_span(buf_peek(&before)));
	if (webtoken_open(url)) {
		webtoken_say(tty, "The browser cannot be started for the token page.\n");
	} else if (readable) {
		webtoken_say(tty, "The browser opens the token page. Copy the text of the page "
		             "(Ctrl+A, Ctrl+C): the token is taken from the clipboard.\n");
		uint64_t const deadline = loop_clock() + WEBTOKEN_COPY * NS;
		while (!token->data && loop_clock() < deadline) {
			struct timespec const second = clk_timespec(NS);
			(void)clock_nanosleep(CLOCK_MONOTONIC, 0, &second, nullptr);
			if (webtoken_clipboard(out))
				break;
			struct cstr_view const found = jwt_find(cstr_view_span(buf_peek(out)));
			if (found.data && !cstr_view_equal(found, old))
				*token = found;
		}
		if (token->data) {
			struct buf none = buf();
			char const *argv[] = { "clip.exe", nullptr };
			(void)webtoken_run(WEBTOKEN_CLIP, argv, WEBTOKEN_READ, &none);
			buf_fini(&none);
		}
	} else {
		webtoken_say(tty, "The browser opens the token page.\n");
	}
	buf_wipe(&before);
	buf_fini(&before);
}

/* Step 3: reads a line from the terminal @p tty without an echo, and
 * writes the token of the line to @p *token, a view of @p out, or an empty
 * view. A line of a terminal in its usual mode ends after 4095 bytes,
 * which is less than a token can be, so the line is read byte by byte.
 */
static void
webtoken_paste (int               tty,
                struct buf       *out,
                struct cstr_view *token)
{
	*token = cstr_view(nullptr);
	struct termios was;
	if (tcgetattr(tty, &was))
		return;

	struct termios quiet = was;
	quiet.c_lflag &= ~(tcflag_t)(ECHO | ICANON);
	quiet.c_cc[VMIN] = 1;
	quiet.c_cc[VTIME] = 0;
	webtoken_say(tty, "Paste the token, or the text of the page, and press Enter: ");
	buf_clear(out);
	if (!tcsetattr(tty, TCSAFLUSH, &quiet)) {
		char c;
		while (read(tty, &c, 1) == 1 && c != '\n' && c != '\r' &&
		       !buf_put(out, &c, 1, WEBTOKEN_OUT))
			;
		(void)tcsetattr(tty, TCSAFLUSH, &was);
	}
	webtoken_say(tty, "\n");
	*token = jwt_find(cstr_view_span(buf_peek(out)));
}

int
main (int    argc,
      char **argv)
{
	char const *url = argc == 2 ? argv[1] : "";
	int tty = open("/dev/tty", O_RDWR | O_CLOEXEC | O_NOCTTY);
	if (!cstr_view_starts(cstr_view(url), CSTR_VIEW("https://")) &&
	    !cstr_view_starts(cstr_view(url), CSTR_VIEW("http://"))) {
		fputs("usage: askai-open-browser URL\n"
		      "Prints the JSON Web Token of the page at the http or https URL.\n", stderr);
		util_close(&tty);
		return 2;
	}
	struct buf out = buf();
	struct cstr_view token;
	webtoken_fetch(url, &out, &token);
	if (!token.data)
		webtoken_copy(url, tty, &out, &token);
	if (!token.data && tty >= 0)
		webtoken_paste(tty, &out, &token);
	int e = token.data ? util_write_all(STDOUT_FILENO, token.data, token.len) : ENOMSG;
	if (!e)
		e = util_write_all(STDOUT_FILENO, "\n", 1);
	if (e == ENOMSG)
		webtoken_say(tty, "No token.\n");
	buf_wipe(&out);
	buf_fini(&out);
	util_close(&tty);
	return !!e;
}
