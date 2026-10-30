/**
 * @file LD_PRELOAD shim for tests/askai.py.  It lets askai see groups that
 * the test machine does not have.
 *
 * getgrnam_r() answers these group names itself, and passes every other
 * name on to the C library:
 * - "peers-big" needs a buffer of at least 2 MiB, and then has gid 4242;
 * - "peers-huge" always needs a larger buffer (ERANGE);
 * - "peers-eio" fails with EIO;
 * - "peers-gone" fails with ENOENT.
 */
#define _GNU_SOURCE

#include <dlfcn.h>
#include <errno.h>
#include <grp.h>
#include <stddef.h>
#include <string.h>

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
	if (!real) {
		/* POSIX makes function and object pointers alike; ISO C does not. */
		void *sym = dlsym(RTLD_NEXT, "getgrnam_r");
		memcpy(&real, &sym, sizeof real);
	}
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
