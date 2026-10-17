/** @file Retained filename completion storage for CLI commands. */
#ifndef TWINCEPTION_SRC_AUTOCOMPLETE_FILE_H_
#define TWINCEPTION_SRC_AUTOCOMPLETE_FILE_H_

#include "autocomplete.h"

#ifdef __cplusplus
# define STD(x) std::x
#else
# define STD(x) x
#endif

#ifdef __cplusplus
extern "C" {
#endif

struct autocomplete_file;

extern struct autocomplete_file *
autocomplete_file_create (int *e);

extern void
autocomplete_file_destroy (struct autocomplete_file **pp);

/**
 * Completes one filename after a matching command (including its trailing
 * space). Results and scratch buffers remain owned by file until the next
 * call or destruction; input spans must not alias that storage. Keeps
 * capacity, returns at most 256 matches / 16 MiB of text. A missing or
 * unreadable directory has no matches. Returns 0 or an error number.
 */
extern int
autocomplete_file_complete (struct autocomplete_file         *file,
                            struct autocomplete_text          line,
                            STD(size_t)                       cursor,
                            struct autocomplete_text const   *cmds,
                            STD(size_t)                       cmd_count,
                            struct autocomplete_match const **matches,
                            STD(size_t)                      *count);

#ifdef __cplusplus
}
#endif

#undef STD

#endif /* TWINCEPTION_SRC_AUTOCOMPLETE_FILE_H_ */
