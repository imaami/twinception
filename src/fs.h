/** @file fs.h
 *
 * Filesystem utility functions.
 */
#ifndef ASKAI_SRC_FS_H_
#define ASKAI_SRC_FS_H_

#include <sys/stat.h>
#include <sys/types.h>

/** @brief Flags of fs_parent_open() and fs_private_dir(). */
enum fs_flag : unsigned {
	FS_PARENTS = 1U << 0,  /**< make the parent directories that are
	                            missing, with mode 0700 */
};

/** @brief Create all parent directories of `path` with mode 0700.
 *
 * The last component of `path` is not created, also if slashes follow it.
 *
 * @note Uses `path` as a work buffer: writes to it, and restores its
 *       contents before the return.
 *
 * @return 0 on success, an error code on failure.
 */
extern int
fs_mkdir_parents (char *path);

/** @brief Open the directory that contains the file `path`.
 *
 * `path` is relative to the directory `dir`. Each directory of `path` is
 * opened in the one before it, without following a symbolic link, so no
 * component of `path` can redirect the walk. With FS_PARENTS in `flags`,
 * a missing directory is made in the same way, and never by its path.
 *
 * `*fd` is the directory, which the caller closes, and `*leaf` is the last
 * component of `path`: the name of the file. After an error `*fd` is -1
 * and `*leaf` is a null pointer. `dir` stays open.
 *
 * @return 0 on success. EINVAL for an empty path, a path that starts with
 *         a slash, a ".." component, a component of more than NAME_MAX
 *         bytes, or a path that does not end with a file name. Otherwise
 *         the error code of fcntl(), openat() or mkdirat(): ENOENT for a
 *         missing directory, ENOTDIR for a symbolic link or another file
 *         that is no directory.
 */
extern int
fs_parent_open (int          dir,
                char const  *path,
                unsigned     flags,
                char const **leaf,
                int         *fd);

/** @brief Tell why the file of `st` is not a private file of type `type`.
 *
 * A private file has the type `type` (S_IFREG or S_IFDIR), the user of
 * the process owns it, no other user may access it, and a regular file
 * has one link.
 *
 * @return A static text with the reason, or a null pointer for a private
 *         file.
 */
extern char const *
fs_private (struct stat const *st,
            mode_t             type);

/** @brief Check that the open file `fd` is a private file of type `type`.
 *
 * @return 0 for a private file, EACCES for another file, or the error
 *         code of fstat().
 */
extern int
fs_private_fd (int    fd,
               mode_t type);

/** @brief Open the private regular file `name` in the directory `dir`.
 *
 * The file is opened with `flags` and without following a symbolic link.
 * A new file gets mode 0600.
 *
 * @note `*fd` stays open after an error of the check, so that the caller
 *       can remove a file that it has just made.
 *
 * @return 0 on success, the error code of openat(), or what
 *         fs_private_fd() returns for the file.
 */
extern int
fs_private_open (int         dir,
                 char const *name,
                 int         flags,
                 int        *fd);

/** @brief Make and open the private directory `path`.
 *
 * A missing directory is made with mode 0700, and with FS_PARENTS in
 * `flags` its missing parents too, as fs_mkdir_parents() makes them. The
 * directory is then opened as `*fd`, and must be private. A symbolic link
 * is followed in the parents but not at the directory itself, also if
 * slashes end `path`.
 *
 * `*why`, unless `why` is a null pointer, is the reason of fs_private()
 * if the directory is not private, and a null pointer otherwise.
 *
 * @note Uses `path` as a work buffer: writes to it, and restores its
 *       contents before the return.
 *
 * @return 0 on success with the directory open as `*fd`. Otherwise `*fd`
 *         is -1: EACCES for a directory that is not private, or the error
 *         code of mkdir(), openat() or fstat().
 */
extern int
fs_private_dir (char         *path,
                unsigned      flags,
                int          *fd,
                char const  **why);

#endif /* ASKAI_SRC_FS_H_ */
