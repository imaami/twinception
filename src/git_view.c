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

/** Consume one newline-terminated line, excluding its newline. */
static int
git_view_line (struct span *input,
               struct span *line)
{
	unsigned char *end = input->len ? memchr(input->ptr, '\n', input->len) : nullptr;
	if (!end)
		return EINVAL;
	*line = (struct span){ input->ptr, (size_t)(end - input->ptr) };
	input->ptr = end + 1;
	input->len -= line->len + 1;
	return 0;
}

/** Takes the next line of @p metadata, which must be an absolute path
 *  without a NUL, and writes a NUL over its newline. Returns the path, or
 *  nullptr if the line is not one.
 */
static char const *
git_view_path (struct span *metadata)
{
	struct span line;
	if (git_view_line(metadata, &line) || !line.len || line.ptr[0] != '/' ||
	    memchr(line.ptr, '\0', line.len))
		return nullptr;
	line.ptr[line.len] = '\0';
	return (char const *)line.ptr;
}

/** Validates the "OID refname" lines of @p refs: each OID is @p width
 *  hexadecimal digits, and each name is in a namespace of GIT_VIEW_REFS,
 *  without a space or a control character.
 */
static int
git_view_refs (struct span refs,
               size_t      width)
{
#define GIT_VIEW_PREFIX(s) CSTR_VIEW_INIT(s),
	static struct cstr_view const prefixes[] = { GIT_VIEW_REFS(GIT_VIEW_PREFIX) };
#undef GIT_VIEW_PREFIX
	while (refs.len) {
		struct span line;
		if (git_view_line(&refs, &line) || line.len <= width + 1 ||
		    line.ptr[width] != ' ' ||
		    !oid_valid((struct cstr_view){ (char const *)line.ptr, width }))
			return EINVAL;
		struct cstr_view const name = { (char const *)line.ptr + width + 1,
		                                line.len - width - 1 };
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
git_view_file (struct git_view *view,
               char const      *name,
               struct span      data)
{
	int fd = openat(view->fd, name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
	                0600);
	if (fd < 0)
		return errno;
	int e = util_write_all(fd, data.ptr, data.len);
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
	for (struct span left = buf_peek(&data), oid; !e && left.len; )
		if (git_view_line(&left, &oid) || oid.len != width ||
		    !oid_valid((struct cstr_view){ (char const *)oid.ptr, oid.len }))
			e = EINVAL;
	if (!e)
		e = git_view_file(view, "shallow", buf_peek(&data));
	buf_fini(&data);
	return e;
}

struct git_view *
git_view_create (struct span metadata,
                 struct span refs,
                 int        *e)
{
	*e = EINVAL;
	char const *objects = git_view_path(&metadata);
	char const *shallow = git_view_path(&metadata);
	struct span format;
	struct span head = {};
	if (!objects || !shallow || git_view_line(&metadata, &format))
		return nullptr;
	enum oid_format const width = oid_format((struct cstr_view){ (char const *)format.ptr,
	                                                             format.len });
	if (!width)
		return nullptr;
	if (metadata.len && (git_view_line(&metadata, &head) || head.len != width ||
	                     !oid_valid((struct cstr_view){ (char const *)head.ptr, head.len })))
		return nullptr;
	if (metadata.len || git_view_refs(refs, width))
		return nullptr;
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
	if (!*e && (mkdirat(view->fd, "refs", 0700) || symlinkat(objects, view->fd, "objects")))
		*e = errno;
	static char const config[] = "[core]\nrepositoryFormatVersion = 0\nbare = true\n";
	static char const config256[] = "[core]\nrepositoryFormatVersion = 1\nbare = true\n"
	                                "[extensions]\nobjectFormat = sha256\n";
	struct span cfg = width == OID_SHA256
		? (struct span){ (unsigned char *)config256, sizeof config256 - 1 }
		: (struct span){ (unsigned char *)config, sizeof config - 1 };
	static char const unborn[] = "ref: refs/askai/unborn\n";
	head = head.len ? (struct span){ head.ptr, head.len + 1 }
	                : (struct span){ (unsigned char *)unborn, sizeof unborn - 1 };
	if (!*e)
		*e = git_view_file(view, "config", cfg);
	if (!*e)
		*e = git_view_file(view, "HEAD", head);
	if (!*e)
		*e = git_view_file(view, "packed-refs", refs);
	if (!*e)
		*e = git_view_shallow(view, shallow, width);
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
