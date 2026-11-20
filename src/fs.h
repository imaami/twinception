/** @file fs.h
 *
 * Filesystem utility functions.
 */
#ifndef ASKAI_SRC_FS_H_
#define ASKAI_SRC_FS_H_


/** @brief Create all parent directories of `path` with mode 0700.
 *
 * @note Uses `path` as a work buffer and may modify its contents.
 *
 * @return 0 on success, an error code on failure.
 */
extern int
fs_mkdir_parents (char *path);

#endif /* ASKAI_SRC_FS_H_ */
