/** @file fs.h
 *
 * Filesystem utility functions.
 */
#ifndef ASKAI_SRC_FS_H_
#define ASKAI_SRC_FS_H_


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

#endif /* ASKAI_SRC_FS_H_ */
