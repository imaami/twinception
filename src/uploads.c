/** @file uploads.c
 *
 * The files that askai uploaded for notes.
 */
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "oid.h"
#include "uploads.h"

/* A file of the table. Its expires is 0 while the place is free. */
struct uploads_file {
	int64_t    expires;
	uint64_t   owner;
	uint64_t   bytes;
	uint16_t   uri_len;
	struct oid id;
	char       uri[UPLOADS_URI];
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

/* Returns the file of @p owner for the note id @p id in @p u, or nullptr.
 * The caller must have locked u->lock.
 */
static struct uploads_file *
uploads_lookup (struct uploads   *u,
                uint64_t          owner,
                struct oid const *id)
{
	for (size_t i = 0; i < UPLOADS_MAX; ++i) {
		struct uploads_file *f = &u->files[i];
		if (f->expires && f->owner == owner && oid_equal(&f->id, id))
			return f;
	}
	return nullptr;
}

int
uploads_find (struct uploads   *uploads,
              uint64_t          owner,
              struct oid const *id,
              int64_t           now,
              char              uri[UPLOADS_URI],
              size_t           *len,
              size_t           *bytes)
{
	if (!uploads || !id || !uri || !len || !bytes)
		return EINVAL;
	if (!oid_len(id))
		return ENOENT;
	int e = ENOENT;
	pthread_mutex_lock(&uploads->lock);
	struct uploads_file const *f = uploads_lookup(uploads, owner, id);
	if (f && f->expires - UPLOADS_MARGIN > now) {
		memcpy(uri, f->uri, f->uri_len);
		*len = f->uri_len;
		*bytes = f->bytes;
		e = 0;
	}
	pthread_mutex_unlock(&uploads->lock);
	return e;
}

int
uploads_keep (struct uploads   *uploads,
              uint64_t          owner,
              struct oid const *id,
              struct cstr_view  uri,
              size_t            bytes,
              int64_t           expires,
              int64_t           now)
{
	if (!uploads || !id || !oid_len(id) || !uri.data || !uri.len || uri.len > UPLOADS_URI ||
	    expires <= now)
		return EINVAL;
	pthread_mutex_lock(&uploads->lock);
	struct uploads_file *to = uploads_lookup(uploads, owner, id);
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
	to->uri_len = (uint16_t)uri.len;
	to->id = *id;
	memcpy(to->uri, uri.data, uri.len);
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
