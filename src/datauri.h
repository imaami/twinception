/** @file datauri.h
 *
 * RFC 2397 data: URIs, by which an MCP client hands askai a file
 * inline (the extension io.modelcontextprotocol/files): data:, an optional
 * media type with parameters, ";base64" or not, a comma, and the data,
 * base64 in the standard alphabet, or else percent-encoded.
 */
#ifndef TWINCEPTION_SRC_DATAURI_H_
#define TWINCEPTION_SRC_DATAURI_H_

#include <stddef.h>
#include <stdint.h>

#include "cstr_view.h"

/** A data: URI that datauri_init() checked. */
struct datauri {
	struct cstr_view encoded;  /**< the encoded data, after the comma */
	size_t           size;     /**< the bytes of the data once decoded */
	uint64_t         base64;   /**< bool: base64, else percent-encoded;
	                                widened over the padding */
};

/** Checks @p uri as a data: URI and initializes @p d with its parts.
 *  Returns 0, or EINVAL if @p uri is no data: URI or its data is not valid
 *  in its encoding.
 */
extern int
datauri_init (struct datauri   *d,
              struct cstr_view  uri);

/** Decodes the data of @p d, which datauri_init() checked, into the
 *  d->size bytes at @p out.
 */
extern void
datauri_decode (struct datauri const *d,
                unsigned char        *out);

#endif /* TWINCEPTION_SRC_DATAURI_H_ */
