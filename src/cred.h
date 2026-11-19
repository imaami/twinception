/** @file cred.h
 * A private credential file: a JSON object in a file that only its owner
 * can read, read and replaced under a lock that the processes of the owner
 * share.
 */
#ifndef ASKAI_SRC_CRED_H_
#define ASKAI_SRC_CRED_H_

#include <stdint.h>

#include "dstr.h"

struct json_object;

enum cred_flag : uint32_t {
	CRED_CREATE = 1u << 0,  /**< a file that does not exist is an empty
	                             record, and its directories are made */
};

/** An open credential file with its lock. record is the object of the file.
 *  The descriptors are those of the directory of the file and of the lock
 *  file, or -1.
 */
struct cred {
	struct json_object *record;
	struct dstr         name;
	int                 directory;
	int                 lock;
};

/**
 * Opens the credential file @p path, waits up to two seconds for its
 * lock, and reads its record. The file, its lock file @p path.lock and
 * their directory are private to the owner, and no part of @p path may be
 * a symbolic link. @p flags is of enum cred_flag. Returns the file, which
 * cred_fini() closes also after an error, with 0 or the error number in
 * @p *e: EBUSY if another process kept the lock, ENOENT without
 * CRED_CREATE if the file does not exist, EINVAL if the file is no JSON
 * object, or EFBIG if it is more than 1 MiB.
 */
extern struct cred
cred (char const *path,
      uint32_t    flags,
      int        *e);

/** Lets go of the record, the lock and the descriptors of @p file. */
extern void
cred_fini (struct cred *file);

/**
 * Replaces the content of @p file with @p record, through a temporary file
 * in the same directory. @p record can be the record of @p file. Returns
 * 0 or an error number, after which the file is as it was.
 */
extern int
cred_save (struct cred        *file,
           struct json_object *record);

#endif /* ASKAI_SRC_CRED_H_ */
