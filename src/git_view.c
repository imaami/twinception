/** @file Private bare snapshots (git_view.h). */
#define _GNU_SOURCE /* mkdtemp(), openat(), symlinkat() */

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cstr_view.h"
#include "git_view.h"

static char const git_view_template[] = "/tmp/askai-git-XXXXXX";

struct git_view {
	int  fd;
	bool head;
	char directory[sizeof git_view_template];
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

static bool
git_view_oid (struct span oid,
              size_t      width)
{
	if (oid.len != width)
		return false;
	for (size_t i = 0; i < oid.len; ++i)
		if (!(oid.ptr[i] >= '0' && oid.ptr[i] <= '9') &&
		    !(oid.ptr[i] >= 'a' && oid.ptr[i] <= 'f'))
			return false;
	return true;
}

static int
git_view_refs (struct span refs,
               size_t      width)
{
	static struct cstr_view const prefixes[] = {
		CSTR_VIEW("refs/heads/"), CSTR_VIEW("refs/tags/"), CSTR_VIEW("refs/remotes/")
	};
	while (refs.len) {
		struct span line;
		if (git_view_line(&refs, &line) || line.len <= width + 1 ||
		    line.ptr[width] != ' ' ||
		    !git_view_oid((struct span){ line.ptr, width }, width))
			return EINVAL;
		struct span name = { line.ptr + width + 1, line.len - width - 1 };
		size_t p = 0;
		while (p < sizeof prefixes / sizeof *prefixes &&
		       (name.len <= prefixes[p].len ||
		        memcmp(name.ptr, prefixes[p].data, prefixes[p].len)))
			++p;
		if (p == sizeof prefixes / sizeof *prefixes)
			return EINVAL;
		for (size_t i = 0; i < name.len; ++i)
			if (name.ptr[i] <= ' ' || name.ptr[i] == 127)
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
	int e = 0;
	while (data.len) {
		ssize_t n = write(fd, data.ptr, data.len);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0) {
			e = n ? errno : EIO;
			break;
		}
		data.ptr += (size_t)n;
		data.len -= (size_t)n;
	}
	if (close(fd) && !e)
		e = errno;
	fd = -1;
	return e;
}

/** Copy only complete shallow boundaries from the discovered regular file. */
static int
git_view_shallow (struct git_view *view,
                  struct span      path,
                  size_t           width)
{
	char *name = strndup((char const *)path.ptr, path.len);
	if (!name)
		return ENOMEM;
	int fd = open(name, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
	int e = fd < 0 && errno != ENOENT ? errno : 0;
	free(name);
	name = nullptr;
	if (fd < 0)
		return e;
	struct stat st;
	if (fstat(fd, &st))
		e = errno;
	else if (!S_ISREG(st.st_mode) || st.st_size < 0 || st.st_size > 4 << 20)
		e = EINVAL;
	size_t size = e ? 0 : (size_t)st.st_size;
	unsigned char *data = e ? nullptr : malloc(size + 1);
	if (!e && !data)
		e = ENOMEM;
	size_t used = 0;
	while (!e) {
		ssize_t n = read(fd, data + used, size + 1 - used);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0) {
			if (n)
				e = errno;
			break;
		}
		used += (size_t)n;
		if (used > size)
			e = EFBIG;
	}
	if (close(fd) && !e)
		e = errno;
	fd = -1;
	struct span left = { data, used };
	while (!e && left.len) {
		struct span oid;
		if (git_view_line(&left, &oid) || !git_view_oid(oid, width))
			e = EINVAL;
	}
	if (!e)
		e = git_view_file(view, "shallow", (struct span){ data, used });
	free(data);
	data = nullptr;
	return e;
}

struct git_view *
git_view_create (struct span metadata,
                 struct span refs,
                 int        *e)
{
	struct span objects, shallow, format, head = {};
	*e = EINVAL;
	if (git_view_line(&metadata, &objects) || !objects.len || objects.ptr[0] != '/' ||
	    memchr(objects.ptr, '\0', objects.len) || git_view_line(&metadata, &shallow) ||
	    !shallow.len || shallow.ptr[0] != '/' || memchr(shallow.ptr, '\0', shallow.len) ||
	    git_view_line(&metadata, &format))
		return nullptr;
	bool sha256 = format.len == 6 && !memcmp(format.ptr, "sha256", 6);
	if (!sha256 && !(format.len == 4 && !memcmp(format.ptr, "sha1", 4)))
		return nullptr;
	size_t width = sha256 ? 64 : 40;
	if (metadata.len && (git_view_line(&metadata, &head) || !git_view_oid(head, width)))
		return nullptr;
	if (metadata.len || git_view_refs(refs, width))
		return nullptr;
	char *path = strndup((char const *)objects.ptr, objects.len);
	struct git_view *view = malloc(sizeof *view);
	if (!path || !view) {
		free(path);
		path = nullptr;
		free(view);
		view = nullptr;
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
	if (!*e && (mkdirat(view->fd, "refs", 0700) || symlinkat(path, view->fd, "objects")))
		*e = errno;
	free(path);
	path = nullptr;
	static char const config[] = "[core]\nrepositoryFormatVersion = 0\nbare = true\n";
	static char const config256[] = "[core]\nrepositoryFormatVersion = 1\nbare = true\n"
	                                "[extensions]\nobjectFormat = sha256\n";
	struct span cfg = sha256
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
		for (size_t i = 0; i < sizeof files / sizeof *files; ++i)
			(void)unlinkat(view->fd, files[i], 0);
		(void)unlinkat(view->fd, "refs", AT_REMOVEDIR);
		(void)close(view->fd);
		view->fd = -1;
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
