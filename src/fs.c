/** @file fs.c
 *
 * Filesystem utility functions.
 */
#include <errno.h>
#include <sys/stat.h>

#include "fs.h"

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
