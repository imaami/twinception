/** @file git_view.c
 *
 * Private bare snapshots.
 */
#define _GNU_SOURCE /* mkdtemp(), openat(), symlinkat() */

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cstr_view.h"
#include "git_view.h"
#include "oid.h"
#include "util.h"

static char const git_view_template[] = "/tmp/askai-git-XXXXXX";

/** The most bytes of a shallow file. */
enum { GIT_VIEW_SHALLOW = 4 << 20 };

struct git_view {
	int      fd;
	char     directory[sizeof git_view_template];
	uint16_t head;  /**< not 0 if HEAD of the snapshot is a commit */
};

/** Takes the next line of @p rest into @p path. Returns 0, or EINVAL if
 *  the line is not an absolute path without a NUL.
 */
static int
git_view_path (struct cstr_view *rest,
               struct cstr_view *path)
{
	return cstr_view_cut(rest, '\n', path) && cstr_view_starts(*path, CSTR_VIEW("/")) &&
	       !memchr(path->data, '\0', path->len) ? 0 : EINVAL;
}

/** Validates the "OID refname" lines of @p refs: each OID is @p width
 *  hexadecimal digits, and each name is in a namespace of GIT_VIEW_REFS,
 *  without a space or a control character.
 */
static int
git_view_refs (struct cstr_view refs,
               size_t           width)
{
#define GIT_VIEW_PREFIX(s) CSTR_VIEW_INIT(s),
	static struct cstr_view const prefixes[] = { GIT_VIEW_REFS(GIT_VIEW_PREFIX) };
#undef GIT_VIEW_PREFIX
	while (refs.len) {
		struct cstr_view line;
		if (!cstr_view_cut(&refs, '\n', &line) || line.len <= width + 1 ||
		    line.data[width] != ' ' || !oid_valid(cstr_view_slice(line, 0, width)))
			return EINVAL;
		struct cstr_view const name = cstr_view_slice(line, width + 1, SIZE_MAX);
		size_t p = 0;
		while (p < array_size(prefixes) &&
		       (name.len <= prefixes[p].len || !cstr_view_starts(name, prefixes[p])))
			++p;
		size_t i = 0;
		while (i < name.len && (unsigned char)name.data[i] > ' ' && name.data[i] != 127)
			++i;
		if (p == array_size(prefixes) || i < name.len)
			return EINVAL;
	}
	return 0;
}

/** Create a new file inside the owned directory, never replacing a path. */
static int
git_view_file (struct git_view  *view,
               char const       *name,
               struct cstr_view  data)
{
	int fd = openat(view->fd, name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
	                0600);
	if (fd < 0)
		return errno;
	int e = util_write_all(fd, data.data, data.len);
	if (close(fd) && !e)
		e = errno;
	fd = -1;
	return e;
}

/** Copy only complete shallow boundaries from the discovered regular file
 *  at @p path.
 */
static int
git_view_shallow (struct git_view *view,
                  char const      *path,
                  size_t           width)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
	if (fd < 0)
		return errno == ENOENT ? 0 : errno;
	struct stat st;
	struct buf data = {};
	int e = fstat(fd, &st) ? errno : !S_ISREG(st.st_mode) ? EINVAL
	      : buf_read(&data, fd, (size_t)st.st_size, GIT_VIEW_SHALLOW);
	util_close(&fd);
	struct span const s = buf_peek(&data);
	struct cstr_view const text = { (char const *)s.ptr, s.len };
	for (struct cstr_view left = text, oid; !e && left.len; )
		if (!cstr_view_cut(&left, '\n', &oid) || oid.len != width || !oid_valid(oid))
			e = EINVAL;
	if (!e)
		e = git_view_file(view, "shallow", text);
	buf_fini(&data);
	return e;
}

struct git_view *
git_view_create (struct span metadata,
                 struct span refs,
                 int        *e)
{
	*e = EINVAL;
	struct cstr_view rest = { (char const *)metadata.ptr, metadata.len };
	struct cstr_view const packed = { (char const *)refs.ptr, refs.len };
	struct cstr_view objects, shallow, format, head = {};
	if (git_view_path(&rest, &objects) || git_view_path(&rest, &shallow) ||
	    !cstr_view_cut(&rest, '\n', &format))
		return nullptr;
	enum oid_format const width = oid_format(format);
	if (!width)
		return nullptr;
	if (cstr_view_cut(&rest, '\n', &head) && (head.len != width || !oid_valid(head)))
		return nullptr;
	if (rest.len || git_view_refs(packed, width))
		return nullptr;
	/* The objects path starts the metadata, and the shallow path follows
	 * its newline. */
	metadata.ptr[objects.len] = '\0';
	metadata.ptr[objects.len + 1 + shallow.len] = '\0';
	struct git_view *view = malloc(sizeof *view);
	if (!view) {
		*e = ENOMEM;
		return nullptr;
	}
	*view = (struct git_view){ .fd = -1, .head = !!head.len };
	memcpy(view->directory, git_view_template, sizeof git_view_template);
	if (!mkdtemp(view->directory)) {
		*e = errno;
		view->directory[0] = '\0';
	} else {
		view->fd = open(view->directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
		*e = view->fd < 0 ? errno : 0;
	}
	if (!*e && (mkdirat(view->fd, "refs", 0700) ||
	            symlinkat(objects.data, view->fd, "objects")))
		*e = errno;
	struct cstr_view const cfg = width == OID_SHA256
		? CSTR_VIEW("[core]\nrepositoryFormatVersion = 1\nbare = true\n"
		            "[extensions]\nobjectFormat = sha256\n")
		: CSTR_VIEW("[core]\nrepositoryFormatVersion = 0\nbare = true\n");
	head = head.len ? (struct cstr_view){ head.data, head.len + 1 }
	                : CSTR_VIEW("ref: refs/askai/unborn\n");
	if (!*e)
		*e = git_view_file(view, "config", cfg);
	if (!*e)
		*e = git_view_file(view, "HEAD", head);
	if (!*e)
		*e = git_view_file(view, "packed-refs", packed);
	if (!*e)
		*e = git_view_shallow(view, shallow.data, width);
	if (*e)
		git_view_destroy(&view);
	return view;
}

void
git_view_destroy (struct git_view **p_view)
{
	if (!p_view || !*p_view)
		return;
	struct git_view *view = *p_view;
	if (view->fd >= 0) {
		static char const *const files[] = {
			"config", "HEAD", "packed-refs", "objects", "shallow"
		};
		for (size_t i = 0; i < array_size(files); ++i)
			(void)unlinkat(view->fd, files[i], 0);
		(void)unlinkat(view->fd, "refs", AT_REMOVEDIR);
		util_close(&view->fd);
	}
	if (*view->directory)
		(void)rmdir(view->directory);
	free(view);
	view = nullptr;
	*p_view = nullptr;
}

char const *
git_view_directory (struct git_view const *view)
{
	return view->directory;
}

bool
git_view_has_head (struct git_view const *view)
{
	return view->head;
}
