/** @file notice.h
 *
 * The [askai: ...] notices that askai appends to the results of a turn's
 * tool calls, after a blank line: the notebook of a read, the number of
 * the call, the two notices about the context, and the notice of the last
 * turn. A turn can copy a result into a note. notice_text_check() checks
 * the text argument of a tool call. For the text of a turn, it stores the
 * length of the text without the notices at its end, which it matches with
 * the same formats, and it refuses a text that contains the markup of a
 * later tool call.
 */
#ifndef ASKAI_SRC_NOTICE_H_
#define ASKAI_SRC_NOTICE_H_

#include <inttypes.h>
#include <stddef.h>

#include "cstr_view.h"

#define NOTICE_NOTEBOOK "[askai: notebook %s]"
#define NOTICE_CALL     "[askai: call c%" PRIu32 "]"
#define NOTICE_COMPACT                                                         \
        "[askai: your context is %" PRIu64 "%% full, %" PRIu64 " of %" PRIu64  \
        " tokens. Note what you need to keep, then call compact.]"
#define NOTICE_REMIND                                                          \
        "[askai: your context is %" PRIu64 "%% full, %" PRIu64 " of %" PRIu64  \
        " tokens. Now write notes of what you have learned. Compaction "       \
        "removes the conversation but not the notes.]"
#define NOTICE_LAST                                                            \
        "[askai: your next turn is your last turn. Answer without calling "    \
        "tools.]"

/**
 * Checks @p text, the argument @p what of a tool call, such as "text" or
 * "claim". The text must be 1 byte to TOOL_TEXT_MAX bytes without NUL.
 * The text of a turn must also contain more than the notices at its end
 * and the blank lines before them, and no markup of a later tool call
 * before them: "</function>" and then "</tool_call>" or "<tool_call>".
 * @p kept is nullptr for the text of a client, which only the first check
 * applies to. For the text of a turn, the function stores the length of
 * the text without those notices in @p *kept.
 *
 * Returns 0, or EINVAL with the refusal in the @p len bytes at @p why.
 */
extern int
notice_text_check (struct cstr_view  text,
                   char const       *what,
                   size_t           *kept,
                   char             *why,
                   size_t            len);

#endif /* ASKAI_SRC_NOTICE_H_ */
