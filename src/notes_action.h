/** @file notes_action.h
 *
 * The list of the actions of the notes tool (src/notes_tool.h), with
 * their constants, their names and their flags. The notes tool and
 * askai-cli read the list. The header includes only cstr_view.h, flag.h
 * and xlist.h, so that a reader of the list needs no header of the store.
 */
#ifndef ASKAI_SRC_NOTES_ACTION_H_
#define ASKAI_SRC_NOTES_ACTION_H_

#include <stdint.h>

#include "cstr_view.h"
#include "flag.h"
#include "xlist.h"

/** The flags of an action, the third column of the lists below. The word
 *  contains the shared bit FLAG_WRITE, so the other bits are FLAG_OWN()
 *  bits (src/flag.h).
 */
enum notes_action_flag : uint16_t {
	NOTES_ACTION_WRITE    = FLAG_WRITE,   /**< writes a note to the notebook
	                                           of the caller, as the caller,
	                                           which needs the member name
	                                           of the caller */
	NOTES_ACTION_SELF     = FLAG_OWN(0),  /**< writes the notebook that the
	                                           call names, which can only
	                                           be the notebook of the
	                                           caller. The flag is not set
	                                           for copy, because the call
	                                           names the notebook that
	                                           copy reads. */
	NOTES_ACTION_FILE     = FLAG_OWN(1),  /**< takes the file argument */
	NOTES_ACTION_OFFSET   = FLAG_OWN(2),  /**< takes the offset argument */
	NOTES_ACTION_REVISION = FLAG_OWN(3),  /**< takes the revision argument */
};

/** The actions of the notes tool: X(ID, name, flags, fetch), the constant
 *  NOTES_ACTION_ID of enum notes_action, the name, the flags of the action
 *  (enum notes_action_flag), and the fetch value of the action. Only
 *  notes_tool.c defines the fetch values, so only an expansion in
 *  notes_tool.c can name the fourth column. members lists the records of
 *  the members (member_list). Every caller can use the actions of
 *  NOTES_ACTION_COMMON_LIST, the first NOTES_ACTION_ATTACH actions of
 *  NOTES_ACTION_ALL_LIST. Only a turn whose upstream takes files can use
 *  attach. The commit message of a write begins with the name, in upper
 *  case at the first letter.
 */
#define NOTES_ACTION_COMMON_LIST(X)                                                                                                             \
        X(ADD,     "add",     NOTES_ACTION_WRITE | NOTES_ACTION_SELF | NOTES_ACTION_FILE,                         0)                            \
        X(APPEND,  "append",  NOTES_ACTION_WRITE | NOTES_ACTION_SELF | NOTES_ACTION_FILE,                         FETCH_REPLACE | FETCH_APPEND) \
        X(REPLACE, "replace", NOTES_ACTION_WRITE | NOTES_ACTION_SELF | NOTES_ACTION_FILE,                         FETCH_REPLACE)                \
        X(DELETE,  "delete",  NOTES_ACTION_WRITE | NOTES_ACTION_SELF | NOTES_ACTION_FILE,                         FETCH_REPLACE | FETCH_DELETE) \
        X(RESTORE, "restore", NOTES_ACTION_WRITE | NOTES_ACTION_SELF | NOTES_ACTION_FILE | NOTES_ACTION_REVISION, FETCH_READ | FETCH_RESTORE)   \
        X(MEMBERS, "members", NOTES_ACTION_OFFSET,                                                                FETCH_MEMBERS)                \
        X(LIST,    "list",    NOTES_ACTION_OFFSET,                                                                FETCH_FILES)                  \
        X(SEARCH,  "search",  NOTES_ACTION_OFFSET,                                                                FETCH_GREP)                   \
        X(HISTORY, "history", NOTES_ACTION_OFFSET,                                                                FETCH_LIST)                   \
        X(READ,    "read",    NOTES_ACTION_FILE | NOTES_ACTION_OFFSET | NOTES_ACTION_REVISION,                    FETCH_READ)                   \
        X(DIFF,    "diff",    NOTES_ACTION_FILE | NOTES_ACTION_OFFSET,                                            FETCH_DIFF)                   \
        X(COPY,    "copy",    NOTES_ACTION_WRITE | NOTES_ACTION_FILE,                                             FETCH_READ | FETCH_COPY)
#define NOTES_ACTION_ALL_LIST(X) NOTES_ACTION_COMMON_LIST(X)                                                                                    \
        X(ATTACH,  "attach",  NOTES_ACTION_FILE,                                                                  FETCH_READ | FETCH_ATTACH)

enum notes_action : uint8_t {
#define NOTES_ACTION_ENUM(id, ...) NOTES_ACTION_##id,
	NOTES_ACTION_ALL_LIST(NOTES_ACTION_ENUM)
#undef NOTES_ACTION_ENUM
};

/** The names of the actions. Each is NUL-terminated. */
static struct cstr_view const notes_action_names[] = { NOTES_ACTION_ALL_LIST(XL_VIEW) };

#endif /* ASKAI_SRC_NOTES_ACTION_H_ */
