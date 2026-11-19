#include <errno.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "compat.h"
#include "util.h"

static force_inline bool
is_http_url (char const *u)
{
	return *u++ == 'h' && *u++ == 't' && *u++ == 't' && *u++ == 'p'
	    && (*u == ':' || (*u++ == 's' && *u == ':'))
	    && *++u == '/' && *++u == '/';
}

static int
spawn_and_wait (char const  *cmd,
                char *const *argv)
{
	extern char **environ;

	pid_t pid = 0;
	int e = posix_spawnp(&pid, cmd, nullptr, nullptr, argv, environ);
	if (e)
		return e;

	while (waitpid(pid, &e, 0) < 0) {
		if (errno != EINTR)
			return errno;
	}

	return (WIFEXITED(e) && !WEXITSTATUS(e)) - 1;
}

static int
open_browser (char const *url)
{
	if (!url)
		return EINVAL;

	if (!is_http_url(url))
		return EPROTO;

	char *argv[4] = { nullptr, nullptr, nullptr, nullptr };

	char const *cmd = getenv("BROWSER");
	if (!cmd || !*cmd) {
		char const *wsl = getenv("WSL_DISTRO_NAME");
		if (wsl && *wsl) {
			cmd = "rundll32.exe";
			argv[1] = (char *)"url.dll,FileProtocolHandler";
			argv[2] = (char *)url;
		} else {
			cmd = "xdg-open";
			argv[1] = (char *)url;
		}
	}

	argv[0] = (char *)cmd;

	int e = spawn_and_wait(cmd, argv);
	if (e == ENOENT && argv[2]) {
		cmd = (char *)"/mnt/c/Windows/System32/rundll32.exe";
		e = spawn_and_wait(cmd, argv);
	}

	return e;
}

int
main (int    argc,
      char **argv)
{
	return open_browser(argc > 1 ? argv[1] : nullptr);
}
