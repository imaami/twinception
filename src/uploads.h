/** @file The files that askai uploaded to the upstream's file store for the
 *  notes that turns attached (src/mcp.c), which later turns reuse until the
 *  store lets them expire.  A file is found by its owner, the client whose
 *  ask call attached it, and by the object id of its note, which names the
 *  note's bytes, and so the bytes that askai uploaded.  An owner finds only
 *  its own files, so that no client learns what another attached.
 *
 * All loops share the table.  It has a mutex, which each function locks
 * for the time of its access.  When the table is full, a new file takes
 * the place of the file that expires first.
 */
#ifndef TWINCEPTION_SRC_UPLOADS_H_
#define TWINCEPTION_SRC_UPLOADS_H_

#include <stddef.h>
#include <stdint.h>

enum {
	UPLOADS_MAX    = 128,       /**< files in the table */
	UPLOADS_ID     = 64,        /**< bytes of an object id, at most */
	UPLOADS_URI    = 256,       /**< bytes of a file's URI, at most */
	UPLOADS_MARGIN = 6 * 3600,  /**< seconds before its expiry from which
	                                 a file is no longer given out */
};

struct uploads;

/** Creates an empty table.  Returns it, or nullptr with the error number
 *  in @p *e. */
extern struct uploads *
uploads_create (int *e);

/** Destroys the table of @p *p_uploads, unless it is nullptr, and sets
 *  @p *p_uploads to nullptr. */
extern void
uploads_destroy (struct uploads **p_uploads);

/**
 * Copies into @p uri the URI of the file of @p owner for the note whose
 * object id is the @p id_len bytes at @p id, and its length in bytes into
 * @p *bytes, if the table has that file and it lasts more than
 * UPLOADS_MARGIN seconds past @p now, in seconds since the epoch.  Returns
 * the length of the URI, or 0 if there is no such file or an argument is
 * not valid.
 */
extern size_t
uploads_find (struct uploads *uploads,
              uint64_t        owner,
              char const     *id,
              size_t          id_len,
              int64_t         now,
              char            uri[UPLOADS_URI],
              size_t         *bytes);

/**
 * Puts into the table the file of @p owner, of @p bytes bytes, whose URI is
 * the @p uri_len bytes at @p uri, for the note whose object id is the
 * @p id_len bytes at @p id, and which expires at @p expires, in seconds
 * since the epoch.  It replaces the file of the same owner and id, else the
 * file that expires first: a free place, a file that has expired, or the
 * file that expires soonest.  Returns 0, or EINVAL if an argument is not
 * valid: an id or a URI that is empty or too long, or an expiry that is
 * not past @p now, in seconds since the epoch.
 */
extern int
uploads_keep (struct uploads *uploads,
              uint64_t        owner,
              char const     *id,
              size_t          id_len,
              char const     *uri,
              size_t          uri_len,
              size_t          bytes,
              int64_t         expires,
              int64_t         now);

/** Removes the files of @p owner from the table, so that their notes are
 *  uploaded again: a turn that had one of them failed, perhaps because the
 *  store no longer has it. */
extern void
uploads_forget (struct uploads *uploads,
                uint64_t        owner);

#endif /* TWINCEPTION_SRC_UPLOADS_H_ */
