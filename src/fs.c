/** @file fs.c
 *
 * Filesystem utility functions.
 */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fs.h"
#include "util.h"

int
fs_mkdir_parents (char *path)
{
	int e = 0;
	char *end = path;

	// Skip any redundant slashes at the beginning of an absolute path
	if (*end == '/') {
		while (*++end == '/')
			++path;
	}

	// Here `path` points either to the separator immediately preceding
	// the first path component of an absolute path, or to the start of
	// a relative path's first path component

	do {
		if (!*end)
			return 0;
	} while (*++end != '/');

	// Here `end` points to the separator following the first path component

	for (char *sep = end; *++end; end = sep) {
		while (*end == '/') {
			if (!*++end)
				return 0;
		}

		// Here `end` points to the start of the next path component and
		// `sep` to the separator following the previous path component

		*sep = '\0';
		e = mkdir(path, 0700) ? errno : 0;
		*sep = '/';
		if (e && e != EEXIST)
			break;
		e = 0;

		for (sep = end; *++sep != '/'; )
			if (!*sep)
				return 0;
	}

	return e;
}

// Opens the directory `name`, relative to the directory `at`, as `*fd`. A
// symbolic link is not followed at the last component of `name`. With
// FS_PARENTS in `flags`, a missing directory is made with mode 0700.
static int
fs_dir_at (int         at,
           char const *name,
           unsigned    flags,
           int        *fd)
{
	enum { HOW = O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW };

	*fd = openat(at, name, HOW);
	if (*fd >= 0)
		return 0;

	int const e = errno;
	if (e != ENOENT || !(flags & FS_PARENTS))
		return e;

	if (mkdirat(at, name, 0700) && errno != EEXIST)
		return errno;

	// Another process can have made the name in the meantime, as a file
	// of any kind. Only this open tells what the name is now.
	*fd = openat(at, name, HOW);
	return *fd < 0 ? errno : 0;
}

int
fs_parent_open (int          dir,
                char const  *path,
                unsigned     flags,
                char const **leaf,
                int         *fd)
{
	*fd = -1;
	*leaf = nullptr;

	struct cstr_view rest = cstr_view(path);
	if (!rest.len || *path == '/')
		return EINVAL;

	char const *const end = rest.data + rest.len;
	struct cstr_view part = {};
	int e = util_path_next(&rest, &part);
	if (e)
		return e;

	int at = fcntl(dir, F_DUPFD_CLOEXEC, 3);
	if (at < 0)
		return errno;

	// Each component is a directory if another component follows it
	for (;;) {
		struct cstr_view next = {};
		e = util_path_next(&rest, &next);
		if (e || !next.len)
			break;

		UTIL_TEXT(NAME_MAX + 1) name;
		e = util_text_store(&name, part);
		if (e)
			break;

		int down = -1;
		e = fs_dir_at(at, name.data, flags, &down);
		if (e)
			break;

		(void)close(at);
		at = down;
		part = next;
	}

	// Here `part` is the last component, which must be a file name that
	// ends the path
	if (!e && (!part.len || part.len > NAME_MAX || part.data + part.len != end))
		e = EINVAL;

	if (e) {
		(void)close(at);
		return e;
	}

	*leaf = part.data;
	*fd = at;
	return 0;
}

char const *
fs_private (struct stat const *st,
            mode_t             type)
{
	if ((st->st_mode & S_IFMT) != type)
		return type == S_IFDIR ? "the file is not a directory"
		                       : "the file is not a regular file";

	if (st->st_uid != geteuid())
		return "askai's user does not own the file";

	if (st->st_mode & (S_IRWXG | S_IRWXO))
		return "the mode of the file lets other users access it";

	if (type == S_IFREG && st->st_nlink != 1)
		return "the file has more than one hard link";

	return nullptr;
}

int
fs_private_fd (int    fd,
               mode_t type)
{
	struct stat st;
	if (fstat(fd, &st))
		return errno;

	return fs_private(&st, type) ? EACCES : 0;
}

int
fs_private_open (int         dir,
                 char const *name,
                 int         flags,
                 int        *fd)
{
	*fd = openat(dir, name, flags | O_CLOEXEC | O_NOFOLLOW, 0600);
	if (*fd < 0)
		return errno;

	return fs_private_fd(*fd, S_IFREG);
}

int
fs_private_dir (char         *path,
                unsigned      flags,
                int          *fd,
                char const  **why)
{
	char const *unused;
	if (!why)
		why = &unused;

	*fd = -1;
	*why = nullptr;

	if (flags & FS_PARENTS) {
		int const e = fs_mkdir_parents(path);
		if (e)
			return e;
	}

	// A slash after the last component would make the kernel follow a
	// symbolic link there, also with O_NOFOLLOW. So the path ends at the
	// last component for the open. The root stays as it is.
	size_t len = strlen(path);
	while (len > 1 && path[len - 1] == '/')
		--len;

	// The directory itself is made whenever it is missing
	char const cut = path[len];
	path[len] = '\0';
	int e = fs_dir_at(AT_FDCWD, path, FS_PARENTS, fd);
	path[len] = cut;
	if (e)
		return e;

	struct stat st;
	if (fstat(*fd, &st)) {
		e = errno;
	} else {
		*why = fs_private(&st, S_IFDIR);
		if (!*why)
			return 0;

		e = EACCES;
	}

	(void)close(*fd);
	*fd = -1;
	return e;
}
