/** @file
 * Retained filename completion storage for CLI commands.
 */
#ifndef TWINCEPTION_SRC_AUTOCOMPLETE_FILE_H_
#define TWINCEPTION_SRC_AUTOCOMPLETE_FILE_H_

#include "autocomplete.h"

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
                            struct cstr_view                  line,
                            size_t                            cursor,
                            struct cstr_view const           *cmds,
                            size_t                            cmd_count,
                            struct autocomplete_match const **matches,
                            size_t                           *count);

#endif /* TWINCEPTION_SRC_AUTOCOMPLETE_FILE_H_ */
