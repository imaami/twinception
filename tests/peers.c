/**
 * @file LD_PRELOAD shim for tests/askai.py. It lets askai see users, groups
 * and Unix socket peers that the test machine does not have, and lets a
 * test keep askai dumpable.
 *
 * getgrnam_r() answers these group names itself, and passes every other
 * name on to the C library:
 * - "peers-big" needs a buffer of at least 2 MiB, and then has gid 4242;
 * - "peers-huge" always needs a larger buffer (ERANGE);
 * - "peers-eio" fails with EIO;
 * - "peers-gone" fails with ENOENT.
 *
 * With PEERS_CRED set, it names a file that holds a uid, a gid and any
 * number of further gids, in decimal and separated by spaces. The shim
 * reads the file at each call of getsockopt() with SO_PEERCRED, which then
 * reports the uid and the gid, and with SO_PEERGROUPS, which then reports
 * the further gids. A test can change the file between connections.
 *
 * With PEERS_SUBUID, PEERS_SUBGID, PEERS_OVERFLOWUID or PEERS_OVERFLOWGID
 * set, fopen() or open() of /etc/subuid, /etc/subgid,
 * /proc/sys/kernel/overflowuid or /proc/sys/kernel/overflowgid opens the
 * file that the variable names.
 *
 * With PEERS_DUMPABLE set, prctl(PR_SET_DUMPABLE) does nothing and returns
 * 0, so that a test can list /proc/PID/fd of askai.
 */
#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <unistd.h>

/* Store the address of the next definition of the function @p name after
 * this library (RTLD_NEXT) in the @p size bytes of the function pointer at
 * @p real, or abort() if dlsym() returns nullptr. POSIX requires the same
 * representation for void * and function pointers, so the memcpy() works.
 * ISO C does not require this.
 */
static void
peers_next (void       *real,
            size_t      size,
            char const *name)
{
	void *sym = dlsym(RTLD_NEXT, name);
	if (!sym)
		abort();
	memcpy(real, &sym, size);
}

int
getgrnam_r (char const    *name,
            struct group  *gr,
            char          *buf,
            size_t         len,
            struct group **result)
{
	static int (*real) (char const *, struct group *, char *, size_t,
	                    struct group **);
	static char none[] = "";
	static char *members[] = { NULL };
	if (!real)
		peers_next(&real, sizeof real, "getgrnam_r");
	*result = NULL;
	if (!strcmp(name, "peers-eio"))
		return EIO;
	if (!strcmp(name, "peers-gone"))
		return ENOENT;
	if (!strcmp(name, "peers-huge"))
		return ERANGE;
	if (strcmp(name, "peers-big"))
		return real(name, gr, buf, len, result);
	size_t n = strlen(name) + 1;
	if (len < (size_t)2 << 20 || len < n)
		return ERANGE;
	memcpy(buf, name, n);
	*gr = (struct group){
		.gr_name = buf,
		.gr_passwd = none,
		.gr_gid = 4242,
		.gr_mem = members
	};
	*result = gr;
	return 0;
}

int
prctl (int option,
       ...)
{
	static int (*real) (int, ...);
	if (!real)
		peers_next(&real, sizeof real, "prctl");
	/* The C library's prctl() reads four more arguments, as this does. */
	va_list ap;
	va_start(ap, option);
	unsigned long arg[4];
	for (size_t i = 0; i < sizeof arg / sizeof *arg; ++i)
		arg[i] = va_arg(ap, unsigned long);
	va_end(ap);
	if (option == PR_SET_DUMPABLE && getenv("PEERS_DUMPABLE"))
		return 0;
	return real(option, arg[0], arg[1], arg[2], arg[3]);
}

/* Read the ids in the file that PEERS_CRED names into @p ids, at most
 * @p max of them. Returns how many there are, or -1 if the file cannot be
 * read.
 */
static long
peers_cred (unsigned long *ids,
            size_t         max)
{
	char const *path = getenv("PEERS_CRED");
	int fd = path ? open(path, O_RDONLY | O_CLOEXEC) : -1;
	if (fd < 0)
		return -1;
	char text[4096];
	ssize_t len = read(fd, text, sizeof text - 1);
	close(fd);
	if (len < 0)
		return -1;
	text[len] = '\0';
	size_t n = 0;
	for (char *s = text, *end; n < max; s = end) {
		errno = 0;
		ids[n] = strtoul(s, &end, 10);
		if (end == s || errno)
			break;
		++n;
	}
	return (long)n;
}

int
getsockopt (int        fd,
            int        level,
            int        name,
            void      *value,
            socklen_t *len)
{
	static int (*real) (int, int, int, void *, socklen_t *);
	if (!real)
		peers_next(&real, sizeof real, "getsockopt");
	unsigned long ids[64];
	long n = level != SOL_SOCKET || (name != SO_PEERCRED && name != SO_PEERGROUPS)
	       ? -1 : peers_cred(ids, sizeof ids / sizeof *ids);
	if (n < 2)
		return real(fd, level, name, value, len);
	if (name == SO_PEERCRED) {
		int e = real(fd, level, name, value, len);
		if (e)
			return e;
		struct ucred *cred = value;
		cred->uid = (uid_t)ids[0];
		cred->gid = (gid_t)ids[1];
		return 0;
	}
	socklen_t need = (socklen_t)((size_t)(n - 2) * sizeof(gid_t));
	if (*len < need) {
		*len = need;
		errno = ERANGE;
		return -1;
	}
	gid_t *groups = value;
	for (long i = 2; i < n; ++i)
		groups[i - 2] = (gid_t)ids[i];
	*len = need;
	return 0;
}

/* The file that the variable of the shim opens in place of @p path, or
 * @p path itself.
 */
static char const *
peers_path (char const *path)
{
	static struct {
		char const *path;
		char const *variable;
	} const files[] = {
		{ "/etc/subuid",                  "PEERS_SUBUID"      },
		{ "/etc/subgid",                  "PEERS_SUBGID"      },
		{ "/proc/sys/kernel/overflowuid", "PEERS_OVERFLOWUID" },
		{ "/proc/sys/kernel/overflowgid", "PEERS_OVERFLOWGID" },
	};
	for (size_t i = 0; i < sizeof files / sizeof *files; ++i) {
		char const *other = getenv(files[i].variable);
		if (other && !strcmp(path, files[i].path))
			return other;
	}
	return path;
}

FILE *
fopen (char const *path,
       char const *mode)
{
	static FILE *(*real) (char const *, char const *);
	if (!real)
		peers_next(&real, sizeof real, "fopen");
	return real(peers_path(path), mode);
}

FILE *
fopen64 (char const *path,
         char const *mode)
{
	static FILE *(*real) (char const *, char const *);
	if (!real)
		peers_next(&real, sizeof real, "fopen64");
	return real(peers_path(path), mode);
}

/* The mode argument of open() and open64(), which comes only with O_CREAT
 * or O_TMPFILE.
 */
#define PEERS_MODE(ap, flags) \
        ((flags) & O_CREAT || ((flags) & O_TMPFILE) == O_TMPFILE ? va_arg(ap, mode_t) : 0)

int
open (char const *path,
      int         flags,
      ...)
{
	static int (*real) (char const *, int, ...);
	if (!real)
		peers_next(&real, sizeof real, "open");
	va_list ap;
	va_start(ap, flags);
	mode_t mode = PEERS_MODE(ap, flags);
	va_end(ap);
	return real(peers_path(path), flags, mode);
}

int
open64 (char const *path,
        int         flags,
        ...)
{
	static int (*real) (char const *, int, ...);
	if (!real)
		peers_next(&real, sizeof real, "open64");
	va_list ap;
	va_start(ap, flags);
	mode_t mode = PEERS_MODE(ap, flags);
	va_end(ap);
	return real(peers_path(path), flags, mode);
}

#undef PEERS_MODE
