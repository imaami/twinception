/** @file Connection-owned, offline completion of MCP tools and session names. */
#ifndef TWINCEPTION_SRC_AUTOCOMPLETE_H_
#define TWINCEPTION_SRC_AUTOCOMPLETE_H_

#ifdef __cplusplus
# include <cstddef>
# define AUTOCOMPLETE_STD std::
#else
# include <stddef.h>
# define AUTOCOMPLETE_STD
#endif

#ifdef __cplusplus
extern "C" {
#endif

struct autocomplete;
struct json_object;

/** A byte-counted UTF-8 string. Input need not have a trailing NUL.
 * Input spans passed to mutators must not alias module-owned mutable storage
 * (borrowed completion lines or participant names). */
struct autocomplete_text {
	char const             *data;
	AUTOCOMPLETE_STD size_t  len;
};

/** Discovery data borrowed until the catalog is changed or destroyed. */
struct autocomplete_tool {
	struct autocomplete_text name;
	struct autocomplete_text title;
	struct autocomplete_text description;
	struct json_object      *schema;
};

/** A full replacement line and the byte offset of its editing cursor. */
struct autocomplete_match {
	struct autocomplete_text line;
	AUTOCOMPLETE_STD size_t   cursor;
};

/** Optional caller-owned diagnostic buffer. len is the number of bytes
 * written, excluding NUL; messages are truncated to capacity - 1. */
struct autocomplete_error {
	char                   *data;
	AUTOCOMPLETE_STD size_t  capacity;
	AUTOCOMPLETE_STD size_t  len;
};

/** Single-thread owned. Keeps scratch storage until destruction. */
extern struct autocomplete *
autocomplete_create (int *e);

extern void
autocomplete_destroy (struct autocomplete **p_autocomplete);

extern void
autocomplete_clear_tools (struct autocomplete *autocomplete);

/** Retains the immutable descriptor. Invalid names are ignored. Returns
 * 0, ENOMEM or E2BIG (4096 tools / 16 MiB of discovery data). */
extern int
autocomplete_add_tool (struct autocomplete *autocomplete,
                       struct json_object  *definition);

extern AUTOCOMPLETE_STD size_t
autocomplete_tool_count (struct autocomplete const *autocomplete);

extern struct autocomplete_tool const *
autocomplete_tool_at (struct autocomplete const *autocomplete,
                      AUTOCOMPLETE_STD size_t     index);

extern struct autocomplete_tool const *
autocomplete_find_tool (struct autocomplete const *autocomplete,
                        struct autocomplete_text   name);

/** Copies, sorts and deduplicates safe names; keeps allocated capacity. */
extern int
autocomplete_set_participants (struct autocomplete            *autocomplete,
                               struct autocomplete_text const *names,
                               AUTOCOMPLETE_STD size_t          count);

extern int
autocomplete_set_notebooks (struct autocomplete            *autocomplete,
                            struct autocomplete_text const *names,
                            AUTOCOMPLETE_STD size_t          count);

extern AUTOCOMPLETE_STD size_t
autocomplete_participant_count (struct autocomplete const *autocomplete);

extern struct autocomplete_text
autocomplete_participant_at (struct autocomplete const *autocomplete,
                             AUTOCOMPLETE_STD size_t     index);

/** No I/O. Matches are borrowed until the next completion or destruction.
 * At most 256 matches and 16 MiB of replacement text (including trailing
 * NULs) are returned. Candidates that exceed this budget are omitted. Input
 * is limited to 16 MiB. Returns 0, EINVAL, E2BIG or ENOMEM. */
extern int
autocomplete_complete (struct autocomplete              *autocomplete,
                       struct autocomplete_text         line,
                       AUTOCOMPLETE_STD size_t           cursor,
                       struct autocomplete_match const **matches,
                       AUTOCOMPLETE_STD size_t           *count);

/** Parses /tool NAME key=VALUE ... or /tool NAME {JSON}. On success the
 * caller owns arguments and borrows tool from the catalog. On failure both
 * are null. Shorthand checks basic types; full JSON is validated by the
 * server. Input is limited to 16 MiB (E2BIG otherwise). Returns 0 or an
 * error number and optionally a diagnostic. */
extern int
autocomplete_parse_call (struct autocomplete             *autocomplete,
                         struct autocomplete_text        line,
                         struct autocomplete_tool const **tool,
                         struct json_object             **arguments,
                         struct autocomplete_error       *diagnostic);

#ifdef __cplusplus
}
#endif

#undef AUTOCOMPLETE_STD

#endif /* TWINCEPTION_SRC_AUTOCOMPLETE_H_ */
