/** @file
 * Connection-owned, offline completion of MCP tools and session names.
 * Input views contain UTF-8 bytes and need not have a trailing NUL. Views
 * passed to mutators must not alias module-owned mutable storage (borrowed
 * completion lines or participant names).
 */
#ifndef TWINCEPTION_SRC_AUTOCOMPLETE_H_
#define TWINCEPTION_SRC_AUTOCOMPLETE_H_

#include "cstr_view.h"

struct autocomplete;
struct json_object;

/** Discovery data borrowed until the catalog is changed or destroyed. */
struct autocomplete_tool {
	struct cstr_view    name;
	struct cstr_view    title;
	struct cstr_view    description;
	struct json_object *schema;
};

/** A full replacement line and the byte offset of its editing cursor. */
struct autocomplete_match {
	struct cstr_view line;
	size_t           cursor;
};

/** Optional caller-owned diagnostic buffer. len is the number of bytes
 * written, excluding NUL; messages are truncated to capacity - 1. */
struct autocomplete_error {
	char   *data;
	size_t  capacity;
	size_t  len;
};

/** Single-thread owned. Keeps scratch storage until destruction. */
extern struct autocomplete *
autocomplete_create (int *e);

extern void
autocomplete_destroy (struct autocomplete **pp);

extern void
autocomplete_clear_tools (struct autocomplete *ac);

/** Retains the immutable descriptor. Invalid names are ignored. Returns
 * 0, ENOMEM or E2BIG (4096 tools / 16 MiB of discovery data). */
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

/** Copies, sorts and deduplicates safe names; keeps allocated capacity. */
extern int
autocomplete_set_participants (struct autocomplete    *ac,
                               struct cstr_view const *names,
                               size_t                  count);

extern int
autocomplete_set_notebooks (struct autocomplete    *ac,
                            struct cstr_view const *names,
                            size_t                  count);

/** Drop cached note paths while retaining their working storage. Tool
 * discovery also clears this cache. */
extern void
autocomplete_clear_note_files (struct autocomplete *ac);

/** Replace a notes tool's file names in one notebook. Empty notebook and
 * "own" select the caller's notebook; "other" selects the opposite side.
 * Other selectors are those returned by the server. Copies, sorts and
 * deduplicates safe UTF-8 paths, including spaces and quotes. All cached
 * notebook lists together are limited to 16 MiB and 4096 lists. Returns
 * 0, EINVAL, ENOMEM or E2BIG. No I/O is performed during completion. */
extern int
autocomplete_set_note_files (struct autocomplete    *ac,
                             struct cstr_view        tool,
                             struct cstr_view        notebook,
                             struct cstr_view const *paths,
                             size_t                  count);

extern size_t
autocomplete_participant_count (struct autocomplete const *ac);

extern struct cstr_view
autocomplete_participant_at (struct autocomplete const *ac,
                             size_t                     index);

/** No I/O. Matches are borrowed until the next completion or destruction.
 * At most 256 matches and 16 MiB of replacement text (including trailing
 * NULs) are returned. Candidates that exceed this budget are omitted. Input
 * is limited to 16 MiB. Returns 0, EINVAL, E2BIG or ENOMEM. */
extern int
autocomplete_complete (struct autocomplete              *ac,
                       struct cstr_view                  line,
                       size_t                            cursor,
                       struct autocomplete_match const **matches,
                       size_t                           *count);

/** Parses /tool NAME key=VALUE ... or /tool NAME {JSON}. On success the
 * caller owns args and borrows tool from the catalog. On failure both
 * are null. Shorthand checks basic types; full JSON is validated by the
 * server. Input is limited to 16 MiB (E2BIG otherwise). Returns 0 or an
 * error number and optionally a diagnostic. */
extern int
autocomplete_parse_call (struct autocomplete             *ac,
                         struct cstr_view                 line,
                         struct autocomplete_tool const **tool,
                         struct json_object             **args,
                         struct autocomplete_error       *diagnostic);

#endif /* TWINCEPTION_SRC_AUTOCOMPLETE_H_ */
