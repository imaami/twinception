/** @file uploads.c
 *
 * The files that askai uploaded for notes.
 */
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "uploads.h"

/* A file of the table.  Its expires is 0 while the place is free. */
struct uploads_file {
	int64_t  expires;
	uint64_t owner;
	uint64_t bytes;
	uint16_t id_len;
	uint16_t uri_len;
	char     id[UPLOADS_ID];
	char     uri[UPLOADS_URI];
};

struct uploads {
	pthread_mutex_t     lock;
	struct uploads_file files[UPLOADS_MAX];
};

struct uploads *
uploads_create (int *e)
{
	if (!e)
		return nullptr;
	struct uploads *u = calloc(1, sizeof *u);
	*e = u ? pthread_mutex_init(&u->lock, nullptr) : ENOMEM;
	if (*e) {
		free(u);
		return nullptr;
	}
	return u;
}

void
uploads_destroy (struct uploads **p_uploads)
{
	struct uploads *u = p_uploads ? *p_uploads : nullptr;
	if (!u)
		return;
	*p_uploads = nullptr;
	pthread_mutex_destroy(&u->lock);
	free(u);
}

/* The file of @p owner for the @p len bytes at @p id in @p u, or nullptr.
 * The caller holds the lock.
 */
static struct uploads_file *
uploads_of (struct uploads *u,
            uint64_t        owner,
            char const     *id,
            size_t          len)
{
	for (size_t i = 0; i < UPLOADS_MAX; ++i) {
		struct uploads_file *f = &u->files[i];
		if (f->expires && f->owner == owner && f->id_len == len &&
		    !memcmp(f->id, id, len))
			return f;
	}
	return nullptr;
}

size_t
uploads_find (struct uploads *uploads,
              uint64_t        owner,
              char const     *id,
              size_t          id_len,
              int64_t         now,
              char            uri[UPLOADS_URI],
              size_t         *bytes)
{
	if (!uploads || !id || !id_len || id_len > UPLOADS_ID || !uri || !bytes)
		return 0;
	size_t len = 0;
	pthread_mutex_lock(&uploads->lock);
	struct uploads_file const *f = uploads_of(uploads, owner, id, id_len);
	if (f && f->expires - UPLOADS_MARGIN > now) {
		len = f->uri_len;
		memcpy(uri, f->uri, len);
		*bytes = f->bytes;
	}
	pthread_mutex_unlock(&uploads->lock);
	return len;
}

int
uploads_keep (struct uploads *uploads,
              uint64_t        owner,
              char const     *id,
              size_t          id_len,
              char const     *uri,
              size_t          uri_len,
              size_t          bytes,
              int64_t         expires,
              int64_t         now)
{
	if (!uploads || !id || !id_len || id_len > UPLOADS_ID || !uri || !uri_len ||
	    uri_len > UPLOADS_URI || expires <= now)
		return EINVAL;
	pthread_mutex_lock(&uploads->lock);
	struct uploads_file *to = uploads_of(uploads, owner, id, id_len);
	if (!to) {
		/* A free place, whose expires is 0, or a file that has expired
		 * expires before every file that has not. */
		to = &uploads->files[0];
		for (size_t i = 1; i < UPLOADS_MAX; ++i)
			if (uploads->files[i].expires < to->expires)
				to = &uploads->files[i];
	}
	to->expires = expires;
	to->owner = owner;
	to->bytes = bytes;
	to->id_len = (uint16_t)id_len;
	to->uri_len = (uint16_t)uri_len;
	memcpy(to->id, id, id_len);
	memcpy(to->uri, uri, uri_len);
	pthread_mutex_unlock(&uploads->lock);
	return 0;
}

void
uploads_forget (struct uploads *uploads,
                uint64_t        owner)
{
	if (!uploads)
		return;
	pthread_mutex_lock(&uploads->lock);
	for (size_t i = 0; i < UPLOADS_MAX; ++i)
		if (uploads->files[i].owner == owner)
			uploads->files[i].expires = 0;
	pthread_mutex_unlock(&uploads->lock);
}
