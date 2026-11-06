/** @file
 * Connection-owned, offline completion of MCP tools and session names.
 * Input views contain UTF-8 bytes and need not have a trailing NUL. Views
 * passed to a mutator must not alias the module-owned storage that it
 * changes: the completion lines, or the names or note paths it replaces.
 */
#ifndef ASKAI_SRC_AUTOCOMPLETE_H_
#define ASKAI_SRC_AUTOCOMPLETE_H_

#include <stdint.h>

#include "cstr_view.h"
#include "dstr.h"

struct autocomplete;
struct json_object;

/** Discovery data borrowed until the catalog is changed or destroyed. */
struct autocomplete_tool {
	struct cstr_view    name;
	struct cstr_view    title;
	struct cstr_view    description;
	struct json_object *schema;
};

enum autocomplete_kind : uint32_t {
	AUTOCOMPLETE_COMMAND,
	AUTOCOMPLETE_TOOL,
	AUTOCOMPLETE_PARAMETER,
	AUTOCOMPLETE_VALUE,
	AUTOCOMPLETE_NAME,
	AUTOCOMPLETE_FILE,
	AUTOCOMPLETE_KIND_COUNT,
};

/** A full replacement line and the byte offset of its editing cursor.
 * label_begin starts the suggested token, excluding already entered context.
 * The displayed label ends at cursor, minus one appended separator space.
 */
struct autocomplete_match {
	struct cstr_view       line;
	size_t                 cursor;
	uint32_t               label_begin;
	enum autocomplete_kind kind;
};

/** AUTOCOMPLETE_MATCHES is the maximum number of matches of a completion.
 *  AUTOCOMPLETE_BYTES is the maximum size of the lines of the matches with
 *  their NULs, of the input of a completion and of the discovery data.
 */
enum {
	AUTOCOMPLETE_MATCHES = 256,
	AUTOCOMPLETE_BYTES   = 16 << 20,
};

/** The matches of a completion and their lines. The next completion reuses
 *  the storage.
 */
struct autocomplete_matches {
	struct autocomplete_match matches[AUTOCOMPLETE_MATCHES];
	struct dstr               lines;  /**< each line and a NUL */
	size_t                    count;
	size_t                    begin;  /**< the least label_begin */
};

/** Empty @p set for the next completion. */
extern void
autocomplete_matches_clear (struct autocomplete_matches *set);

/** Add the concatenation of the @p n views at @p parts to @p set as the
 *  line of a match, with its cursor at @p cursor and its label from
 *  @p label_begin. A line that does not fit in the set is not added.
 *  Returns 0 or ENOMEM.
 */
extern int
autocomplete_matches_add (struct autocomplete_matches *set,
                          struct cstr_view const      *parts,
                          size_t                       n,
                          size_t                       cursor,
                          size_t                       label_begin,
                          enum autocomplete_kind       kind);

/** Point the matches at their lines, sort them by their lines from the
 *  least label_begin, then by cursor, and drop duplicates. Returns the
 *  matches, and their number in @p *count.
 */
extern struct autocomplete_match const *
autocomplete_matches_finish (struct autocomplete_matches *set,
                             size_t                      *count);

/** Single-thread owned. Keeps scratch storage until destruction. */
extern struct autocomplete *
autocomplete_create (int *e);

extern void
autocomplete_destroy (struct autocomplete **pp);

extern void
autocomplete_clear_tools (struct autocomplete *ac);

/** Retains the immutable descriptor. Invalid names are ignored. Returns
 * 0, ENOMEM or E2BIG (4096 tools / 16 MiB of discovery data).
 */
extern int
autocomplete_add_tool (struct autocomplete *ac,
                       struct json_object  *def);

extern size_t
autocomplete_tool_count (struct autocomplete const *ac);

extern struct autocomplete_tool const *
autocomplete_tool_at (struct autocomplete const *ac,
                      size_t                     index);

extern struct autocomplete_tool const *
autocomplete_find_tool (struct autocomplete const *ac,
                        struct cstr_view           name);

enum autocomplete_list : uint32_t {
	/** Names for chat mentions: only the participants present in this
	 * chat, not identities discovered in storage. */
	AUTOCOMPLETE_CHAT_PARTICIPANTS,
	/** Known notebook participants, for tool arguments and the listing. */
	AUTOCOMPLETE_PARTICIPANTS,
	/** Notebook selectors, for tool arguments. */
	AUTOCOMPLETE_NOTEBOOKS,
	AUTOCOMPLETE_LISTS,
};

/** Copies, sorts and deduplicates the names of a list. Keeps allocated
 * capacity. Returns 0, EINVAL or ENOMEM.
 */
extern int
autocomplete_set_names (struct autocomplete    *ac,
                        enum autocomplete_list  list,
                        struct cstr_view const *names,
                        size_t                  count);

/** Drop cached note paths while retaining their working storage. Tool
 * discovery also clears this cache.
 */
extern void
autocomplete_clear_note_files (struct autocomplete *ac);

/** Replace a notes tool's file names in one notebook. Empty notebook and
 * "own" select the caller's notebook; "other" selects the opposite side.
 * Other selectors are those returned by the server. Copies, sorts and
 * deduplicates safe UTF-8 paths, including spaces and quotes. All cached
 * notebook lists together are limited to 16 MiB and 4096 lists. Returns
 * 0, EINVAL, ENOMEM or E2BIG. No I/O is performed during completion.
 */
extern int
autocomplete_set_note_files (struct autocomplete    *ac,
                             struct cstr_view        tool,
                             struct cstr_view        notebook,
                             struct cstr_view const *paths,
                             size_t                  count);

extern size_t
autocomplete_list_count (struct autocomplete const *ac,
                         enum autocomplete_list     list);

/** The name at @p index of the sorted, unique names of @p list, borrowed
 *  until the list is set again, or an empty view.
 */
extern struct cstr_view
autocomplete_list_at (struct autocomplete const *ac,
                      enum autocomplete_list     list,
                      size_t                     index);

/** No I/O. Matches are borrowed until the next completion or destruction.
 * At most 256 matches and 16 MiB of replacement text (including trailing
 * NULs) are returned. Candidates that exceed this budget are omitted. Input
 * is limited to 16 MiB. Returns 0, EINVAL, E2BIG or ENOMEM.
 */
extern int
autocomplete_complete (struct autocomplete              *ac,
                       struct cstr_view                  line,
                       size_t                            cursor,
                       struct autocomplete_match const **matches,
                       size_t                           *count);

/** Whether text is nonempty UTF-8 without C0, DEL or C1 controls; with
 * identifier, also without spaces, quotes, backslashes or '='.
 */
extern bool
autocomplete_safe (struct cstr_view text,
                   bool             identifier);

/** Parses /tool NAME key=VALUE ... or /tool NAME {JSON}. On success the
 * caller owns args and borrows tool from the catalog. On failure both
 * are null. Shorthand checks basic types; full JSON is validated by the
 * server. Input is limited to 16 MiB (E2BIG otherwise). Returns 0 or an
 * error number. If @p why is not nullptr, it receives the diagnostic as
 * text of at most @p size bytes with its NUL, which util_format() cuts
 * with "...".
 */
extern int
autocomplete_parse_call (struct autocomplete             *ac,
                         struct cstr_view                 line,
                         struct autocomplete_tool const **tool,
                         struct json_object             **args,
                         char                            *why,
                         size_t                           size);

#endif /* ASKAI_SRC_AUTOCOMPLETE_H_ */
