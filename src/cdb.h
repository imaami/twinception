/** @file cdb.h
 *
 * The compile commands of a repository, from which askai-code (src/cxi.h)
 * makes the clang command line for a file of the exported tree of a commit.
 */
#ifndef ASKAI_SRC_CDB_H_
#define ASKAI_SRC_CDB_H_

#include "cstr_view.h"

struct dstr;
struct json_object;

/** The real path of the top of the repository and the exported tree. */
struct cdb_map {
	struct cstr_view top;
	struct cstr_view tree;
};

/** Writes @p path, against @p dir if relative, to @p out without ".", ".."
 *  and empty components. Returns 0 or ENOMEM.
 */
extern int
cdb_norm (struct dstr      *out,
          struct cstr_view  dir,
          struct cstr_view  path);

/**
 * Writes to @p words, each word followed by a NUL, the command line of the
 * entry of @p cmds, the array of a compile_commands.json, for @p rel, a .c
 * or .h path relative to the top. clang is the program, paths at or under
 * the top are mapped into the tree, options with a path above the top, the
 * input file and the options that write files are removed, and the path of
 * the file in the tree is the last word. A header takes the entry with the
 * longest common directory and -x c-header. Returns 0, ENOENT for a .c file
 * without an entry, EINVAL if no entry with "arguments" is for a file under
 * the top, or ENOMEM.
 */
extern int
cdb_command (struct cdb_map const *map,
             struct json_object   *cmds,
             struct cstr_view      rel,
             struct dstr          *words);

#endif /* ASKAI_SRC_CDB_H_ */
