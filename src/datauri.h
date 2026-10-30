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

/** A data: URI that datauri_parse() checked. */
struct datauri {
	char const *data;    /**< the encoded data, after the comma */
	size_t      len;     /**< its bytes */
	size_t      size;    /**< its bytes once decoded */
	uint64_t    base64;  /**< bool: base64, else percent-encoded; widened
	                          over the padding */
};

/** Checks the @p len bytes at @p uri as a data: URI, into @p d: 0, or
 *  EINVAL if it is none or its data is not valid in its encoding.
 */
extern int
datauri_parse (char const     *uri,
               size_t          len,
               struct datauri *d);

/** Decodes the data of @p d, which datauri_parse() checked, into the
 *  d->size bytes at @p out.
 */
extern void
datauri_decode (struct datauri const *d,
                unsigned char        *out);

#endif /* TWINCEPTION_SRC_DATAURI_H_ */
