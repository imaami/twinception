/** @file notice.h
 *
 * The [askai: ...] notices that askai appends to the results of a turn's
 * tool calls, after a blank line: the notebook of a read, the two notices
 * about the context, and the notice of the last turn. A turn can copy a
 * result into a note, and notice_kept() finds the notices at the end of
 * such a text by the same formats.
 */
#ifndef ASKAI_SRC_NOTICE_H_
#define ASKAI_SRC_NOTICE_H_

#include <inttypes.h>
#include <stddef.h>

#define NOTICE_NOTEBOOK "[askai: notebook %s]"
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
 * Returns the length of the @p len bytes at @p text without the lines at
 * its end that are notices. The kept text ends with the first line feed
 * after its last non-blank byte. It is empty if only spaces, tabs and
 * line ends come before the notices. Returns @p len if the text does not
 * end with a notice, and 0 for a null @p text.
 */
extern size_t
notice_kept (char const *text,
             size_t      len);

#endif /* ASKAI_SRC_NOTICE_H_ */
