/** @file
 * Filename completion with retained, byte-counted work buffers.
 */
#define _DEFAULT_SOURCE

#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "autocomplete_file.h"
#include "compat.h"
#include "dstr.h"
#include "jb.h"

struct autocomplete_file {
	struct autocomplete_matches found;
	struct dstr                 directory;
	struct cstr_view            home;  /**< $HOME, which does not change */
};

struct autocomplete_file *
autocomplete_file_create (int *const e)
{
	struct autocomplete_file *file = malloc(sizeof *file);
	if (!file) {
		*e = ENOMEM;
		return nullptr;
	}
	struct cstr_view const home = cstr_view(getenv("HOME"));
	*file = (struct autocomplete_file){.found.lines = dstr(nullptr), .directory = dstr(nullptr),
	                                   .home = jb_is_text(home) ? home : (struct cstr_view){}};
	*e = 0;
	return file;
}

void
autocomplete_file_destroy (struct autocomplete_file **const pp)
{
	if (!pp || !*pp)
		return;
	struct autocomplete_file *file = *pp;
	dstr_cleanup(&file->found.lines);
	dstr_cleanup(&file->directory);
	free(file);
	*pp = nullptr;
}

int
autocomplete_file_expand (struct autocomplete_file const *const file,
                          struct cstr_view const                path,
                          struct dstr *const                    dest)
{
	bool const tilde = file->home.len &&
	                   (cstr_view_equal(path, CSTR_VIEW("~")) || cstr_view_starts(path, CSTR_VIEW("~/")));
	if (tilde && cstr_view_cat(file->home, dest))
		return ENOMEM;
	return cstr_view_cat(cstr_view_slice(path, tilde, SIZE_MAX), dest);
}

static bool
autocomplete_file_safe (struct cstr_view const name)
{
	return !cstr_view_equal(name, CSTR_VIEW(".")) && !cstr_view_equal(name, CSTR_VIEW("..")) &&
	       autocomplete_safe(name, false);
}

int
autocomplete_file_complete (struct autocomplete_file *const         file,
                            struct cstr_view const                  line,
                            size_t const                            cursor,
                            struct cstr_view const                  command,
                            struct autocomplete_match const **const matches,
                            size_t *const                           count)
{
	if (!matches || !count)
		return EINVAL;

	*matches = nullptr;
	*count = 0;

	if (line.len > AUTOCOMPLETE_BYTES)
		return E2BIG;
	if (!file || !command.data || !command.len || cursor > line.len || !jb_is_text(line))
		return EINVAL;

	if (cursor < command.len || !cstr_view_starts(line, command))
		return 0;

	struct cstr_view const prefix = cstr_view_slice(line, command.len, cursor - command.len);
	char const *const slash = memchr(line.data + cursor, '/', line.len - cursor);
	struct cstr_view const suffix = slash ? cstr_view_slice(line, (size_t)(slash - line.data) + 1, SIZE_MAX)
	                                      : (struct cstr_view){};
	size_t directory_len = prefix.len;
	while (directory_len && prefix.data[directory_len - 1] != '/')
		--directory_len;
	struct cstr_view const directory = cstr_view_slice(prefix, 0, directory_len);
	struct cstr_view const basename = cstr_view_slice(prefix, directory_len, SIZE_MAX);
	/* The label of each match starts after the command and the directory. */
	size_t const base = command.len + directory.len;
	autocomplete_matches_clear(&file->found);
	if (cstr_view_equal(prefix, CSTR_VIEW("~"))) {
		struct cstr_view const parts[] = { command, prefix, CSTR_VIEW("/"), suffix };
		int const e = autocomplete_matches_add(&file->found, parts, array_size(parts),
		                                       base + prefix.len + 1, base, AUTOCOMPLETE_FILE);
		if (!e)
			*matches = autocomplete_matches_finish(&file->found, count);
		return e;
	}
	(void)dstr_clear(&file->directory);
	if (autocomplete_file_expand(file, directory_len ? directory : CSTR_VIEW("."), &file->directory))
		return ENOMEM;

	DIR *dir = opendir(dstr_get(&file->directory));
	if (!dir)
		return errno == ENOMEM ? ENOMEM : 0;
	int e = 0;
	while (file->found.count < array_size(file->found.matches)) {
		errno = 0;
		struct dirent *const entry = readdir(dir);
		if (!entry) {
			e = errno;
			break;
		}
		struct cstr_view const name = cstr_view(entry->d_name);
		if (!cstr_view_starts(name, basename) || !autocomplete_file_safe(name))
			continue;

		bool is_directory = entry->d_type == DT_DIR;
		if (entry->d_type == DT_UNKNOWN || entry->d_type == DT_LNK) {
			struct stat info;
			is_directory = !fstatat(dirfd(dir), entry->d_name, &info, 0) && S_ISDIR(info.st_mode);
		}
		if (slash && !is_directory)
			continue;
		struct cstr_view const parts[] = {
			command, directory, name, is_directory ? CSTR_VIEW("/") : CSTR_VIEW(" "), suffix,
		};
		e = autocomplete_matches_add(&file->found, parts, array_size(parts), base + name.len + 1,
		                             base, AUTOCOMPLETE_FILE);
		if (e)
			break;
	}

	if (closedir(dir) && !e)
		e = errno;
	if (e)
		return e;

	*matches = autocomplete_matches_finish(&file->found, count);
	return 0;
}
