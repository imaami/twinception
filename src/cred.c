/** @file cred.c
 * A private credential file under a lock.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "buf.h"
#include "clk.h"
#include "cred.h"
#include "cstr_view.h"
#include "dstr.h"
#include "fs.h"
#include "hexid.h"
#include "jb.h"
#include "util.h"

enum { CRED_LIMIT = 1 << 20 };

void
cred_fini (struct cred *file)
{
	json_object_put(file->record);
	file->record = nullptr;
	dstr_cleanup(&file->name);
	util_close(&file->lock_fd);
	util_close(&file->dir_fd);
}

/* The credential and lock share an opened directory. No path component
 * may redirect the transaction through a symbolic link.
 */
struct cred
cred (char const *path,
      uint32_t    flags,
      int        *e)
{
	struct cred file = {
		.name    = DSTR_INIT,
		.dir_fd  = -1,
		.lock_fd = -1,
	};

	do {
		if (!path)
			*e = EFAULT;
		else if (!*path)
			*e = EINVAL;
		else
			break;
		return file;
	} while (0);

	struct dstr scratch = dstr(path);
	size_t len = dstr_len(&scratch);
	if (!len) {
		*e = ENOMEM;
		return file;
	}

	bool const create = flags & CRED_CREATE;
	*e = create ? fs_mkdir_parents((char *)dstr_raw(&scratch)) : 0;
	int base = *e ? -1 : open(*path == '/' ? "/" : ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (!*e && base < 0)
		*e = errno;
	char const *leaf = nullptr;
	if (!*e)
		*e = util_parent_open(base, path + (*path == '/'), &leaf, &file.dir_fd);
	util_close(&base);
	if (!*e)
		*e = util_private_fd(file.dir_fd, S_IFDIR);
	if (!*e)
		*e = cstr_view_store(cstr_view(leaf), &file.name);
	dstr_clear(&scratch);
	if (!*e && !dstr_printf(&scratch, "%s.lock", dstr_get(&file.name)))
		*e = ENOMEM;
	if (!*e)
		*e = util_private_open(file.dir_fd, dstr_get(&scratch), O_RDWR | O_CREAT | O_NONBLOCK, &file.lock_fd);
	uint64_t start = clk_monotonic_ns();
	while (!*e && flock(file.lock_fd, LOCK_EX | LOCK_NB)) {
		if (errno != EINTR && errno != EWOULDBLOCK) {
			*e = errno;
			break;
		}
		uint64_t now = clk_monotonic_ns();
		if (!start || !now || now - start >= 2 * NS) {
			*e = start && now ? EBUSY : EIO;
			break;
		}
		struct timespec delay = clk_timespec(20 * MS);
		int wait = clock_nanosleep(CLOCK_MONOTONIC, 0, &delay, nullptr);
		if (wait && wait != EINTR)
			*e = wait;
	}
	int fd = -1;
	if (!*e)
		*e = util_private_open(file.dir_fd, dstr_get(&file.name), O_RDONLY | O_NONBLOCK, &fd);
	if (*e == ENOENT && create && file.lock_fd >= 0) {
		file.record = json_object_new_object();
		*e = file.record ? 0 : ENOMEM;
		dstr_cleanup(&scratch);
		return file;
	}
	struct stat st;
	if (!*e && fstat(fd, &st))
		*e = errno;
	struct buf bytes = buf();
	if (!*e)
		*e = st.st_size < 0 || st.st_size > CRED_LIMIT ? EFBIG :
		     buf_read(&bytes, fd, (size_t)st.st_size, (size_t)st.st_size);
	util_close(&fd);
	if (!*e) {
		struct span text = buf_peek(&bytes);
		file.record = text.len ? jb_parse((char const *)text.ptr, text.len, JSON_TOKENER_STRICT, 16) : nullptr;
		if (!json_object_is_type(file.record, json_type_object))
			*e = EINVAL;
	}
	buf_wipe(&bytes);
	buf_fini(&bytes);
	dstr_cleanup(&scratch);
	return file;
}

int
cred_save (struct cred *file,
                   struct json_object *record)
{
	struct hexid id = {};
	int e = hexid_make(&id);
	dstr_auto temporary = dstr(nullptr);
	if (!e && !dstr_printf(&temporary, ".cred-%s", id.hex))
		e = ENOMEM;
	struct cstr_view text = jb_wire(record);
	if (!e && !text.data)
		e = ENOMEM;
	int fd = -1;
	if (!e)
		e = util_private_open(file->dir_fd, dstr_get(&temporary), O_WRONLY | O_CREAT | O_EXCL, &fd);
	if (fd < 0)
		return e;
	e = fchmod(fd, 0600) ? errno : util_write_all(fd, text.data, text.len);
	if (!e && fsync(fd))
		e = errno;
	if (fd >= 0 && close(fd) && !e)
		e = errno;
	fd = -1;
	if (!e && (renameat(file->dir_fd, dstr_get(&temporary), file->dir_fd, dstr_get(&file->name)) ||
	           fsync(file->dir_fd)))
		e = errno;
	if (e && dstr_len(&temporary))
		(void)unlinkat(file->dir_fd, dstr_get(&temporary), 0);
	return e;
}
