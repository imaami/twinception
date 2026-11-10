/** @file
 * The gate of UTF-8 text.
 *
 * Bytes that are to be text pass a gate once, where they come in: the
 * c.utf-8 parser takes them apart into characters (RFC 3629), and the gate
 * counts the bytes and the characters of the text as it goes. Past the gate
 * text is text, and nothing checks it again. The parser tells nothing of
 * what a code point is; askai-cli asks utf8proc how wide a character is.
 */
#ifndef ASKAI_SRC_U8_H_
#define ASKAI_SRC_U8_H_

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#include "c.utf-8/src/utf8.h"
#include "cstr_view.h"

/** UTF-8 text without a NUL byte, as the gate measured it. */
struct u8_text {
	union {
		struct cstr_view view;
		struct {
			char const *data;
			size_t      len;    /**< bytes */
		};
	};
	size_t count;               /**< characters */
};

/**
 * A gate that text passes a piece at a time. What has passed is text of
 * @p len bytes and @p count characters, and a character cut short at the end
 * of the last piece, which the next piece goes on with.
 */
struct u8_gate {
	size_t      len;     /**< bytes of the whole characters passed */
	size_t      count;   /**< characters passed */
	struct utf8 parser;  /**< its error is the gate's, once bytes failed it */
};

/** A gate that nothing has passed yet. */
static inline struct u8_gate
u8_gate (void)
{
	return (struct u8_gate){ .parser = utf8() };
}

/**
 * Pass @p bytes through @p gate. Returns 0, or, from the first byte that is
 * not text on, EILSEQ, which a NUL byte is too, or EFAULT for bytes without
 * data. Once the gate has failed, it measures the text before that byte.
 */
extern int
u8_gate_pass (struct u8_gate   *gate,
              struct cstr_view  bytes);

/** Returns 0 if what passed @p gate is text, whole characters, else the
 *  error that failed it, or EILSEQ for a character cut short at the end.
 */
static inline int
u8_gate_end (struct u8_gate const *gate)
{
	return gate->parser.error ? gate->parser.error :
	       utf8_expects_leading_byte(&gate->parser) ? 0 : EILSEQ;
}

/**
 * Measure @p bytes at the gate. Returns the longest start of them that is
 * text, and sets @p *e to 0 if that is all of them, else to the error of
 * u8_gate_end().
 */
extern struct u8_text
u8_text (struct cstr_view  bytes,
         int              *e);

/** Returns 0 if @p bytes are text, else the error of u8_text(). */
static inline int
u8_check (struct cstr_view bytes)
{
	int e;
	(void)u8_text(bytes, &e);
	return e;
}

/**
 * Returns the length of the character that @p bytes begin with, 1 to 4, and
 * its code point in @p *value, or 0 if they do not begin with a whole
 * character of text, which NUL is not.
 */
extern size_t
u8_char (struct cstr_view  bytes,
         uint32_t         *value);

/**
 * Make the @p len bytes at @p data UTF-8 text, in place, each byte that is
 * not of a character becoming '?', but for a character cut short at their
 * end while more bytes follow (@p last false), which those may complete. A
 * NUL byte stays. Returns the bytes made text, all but that character's.
 */
extern size_t
u8_mend (char   *data,
         size_t  len,
         bool    last);

#endif /* ASKAI_SRC_U8_H_ */
