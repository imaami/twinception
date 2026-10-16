/** @file Retained filename completion storage for CLI commands. */
#ifndef TWINCEPTION_SRC_AUTOCOMPLETE_FILE_H_
#define TWINCEPTION_SRC_AUTOCOMPLETE_FILE_H_

#ifdef __cplusplus
# include <cstddef>
# define AUTOCOMPLETE_FILE_STD std::
#else
# include <stddef.h>
# define AUTOCOMPLETE_FILE_STD
#endif

#include "autocomplete.h"

#ifdef __cplusplus
extern "C" {
#endif

struct autocomplete_file;

extern struct autocomplete_file *
autocomplete_file_create (int *e);

extern void
autocomplete_file_destroy (struct autocomplete_file **p_file);

/** Completes one filename after a matching command (including its trailing
 * space). Results and scratch buffers remain owned by file until the next
 * call or destruction; input spans must not alias that storage. Keeps
 * capacity, returns at most 256 matches / 16 MiB of text. A missing or
 * unreadable directory has no matches. Returns 0 or an error number. */
extern int
autocomplete_file_complete (struct autocomplete_file        *file,
                            struct autocomplete_text         line,
                            AUTOCOMPLETE_FILE_STD size_t      cursor,
                            struct autocomplete_text const  *commands,
                            AUTOCOMPLETE_FILE_STD size_t      command_count,
                            struct autocomplete_match const **matches,
                            AUTOCOMPLETE_FILE_STD size_t      *count);

#ifdef __cplusplus
}
#endif

#undef AUTOCOMPLETE_FILE_STD

#endif /* TWINCEPTION_SRC_AUTOCOMPLETE_FILE_H_ */
