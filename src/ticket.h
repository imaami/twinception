/** @file Tickets: capabilities for one transfer of a note over HTTP.
 *
 * A client's notes tool hands out a ticket for an upload of a file into
 * the client's notebook, or for a download of a note, which the client
 * then makes with a program such as curl, without its token and without
 * the text passing through a model.  A ticket names the direction, the
 * principal, the side of the notebook, the thread, the name of the note
 * and the time it expires.  An HMAC-SHA256 of these with a key that askai
 * makes at start, of which the ticket has the first 16 bytes, keeps anyone
 * else from making or changing a ticket.  A ticket is written as URL-safe
 * base64 without padding.  A ticket is good for any number of transfers
 * until it expires: a second upload finds the note there, and is refused.
 */
#ifndef TWINCEPTION_SRC_TICKET_H_
#define TWINCEPTION_SRC_TICKET_H_

#include <stddef.h>
#include <stdint.h>

enum {
	TICKET_THREAD = 32,   /**< hexadecimal digits of a thread's id */
	TICKET_NAME   = 255,  /**< bytes of a note's name, at most */
	TICKET_TEXT   = 432,  /**< bytes of a written ticket, at most, NUL
	                           included */
};

/** What a ticket is for. */
enum ticket_dir : uint8_t {
	TICKET_UPLOAD   = 'u',  /**< PUT the bytes of a new note */
	TICKET_DOWNLOAD = 'd',  /**< GET the bytes of a note */
};

/** The key of the tickets of one run of askai. */
struct ticket_key {
	unsigned char bytes[32];
};

/** One ticket, as ticket_write() writes it and ticket_read() reads it. */
struct ticket {
	uint64_t principal;              /**< 0: anonymous */
	uint64_t expiry;                 /**< seconds since the epoch */
	char     thread[TICKET_THREAD];  /**< hexadecimal digits */
	char     name[TICKET_NAME];
	uint8_t  name_len;
	uint8_t  dir;                    /**< enum ticket_dir */
	uint8_t  side;                   /**< enum notes_side */
};

/** Makes a new random key in @p key; returns 0 or the error of
 *  getrandom(). */
extern int
ticket_key_init (struct ticket_key *key);

/** Writes ticket @p t, signed with @p key, to the @p size bytes at @p out,
 *  NUL-terminated; returns its length, or 0 if an argument is not valid. */
extern size_t
ticket_write (struct ticket_key const *key,
              struct ticket const     *t,
              char                    *out,
              size_t                   size);

/** Reads the ticket of @p len bytes at @p text into @p t.  Returns 0;
 *  EINVAL if it is not a ticket that @p key signed; or ETIMEDOUT if it
 *  expired before @p now, in seconds since the epoch. */
extern int
ticket_read (struct ticket_key const *key,
             char const              *text,
             size_t                   len,
             uint64_t                 now,
             struct ticket           *t);

#endif /* TWINCEPTION_SRC_TICKET_H_ */
