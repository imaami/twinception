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
#include "dstr.h"

enum {
	AUTOCOMPLETE_FILE_MATCHES = 256,
	AUTOCOMPLETE_FILE_LIMIT = 16 << 20,
};

struct autocomplete_file {
	struct autocomplete_match matches[AUTOCOMPLETE_FILE_MATCHES];
	struct dstr               lines; /**< the lines of the matches, each NUL-terminated */
	struct dstr               directory;
	struct dstr               home;
};

struct autocomplete_file *
autocomplete_file_create (int *const e)
{
	struct autocomplete_file *file = malloc(sizeof *file);
	if (!file) {
		*e = ENOMEM;
		return nullptr;
	}
	file->lines = dstr(nullptr);
	file->directory = dstr(nullptr);
	file->home = dstr(nullptr);
	struct cstr_view const home = cstr_view(getenv("HOME"));
	*e = autocomplete_line(home) ? cstr_view_store(home, &file->home) : 0;
	if (*e)
		autocomplete_file_destroy(&file);
	return file;
}

void
autocomplete_file_destroy (struct autocomplete_file **const pp)
{
	if (!pp || !*pp)
		return;
	struct autocomplete_file *file = *pp;
	dstr_cleanup(&file->lines);
	dstr_cleanup(&file->directory);
	dstr_cleanup(&file->home);
	free(file);
	file = nullptr;
	*pp = nullptr;
}

/* Append the line of a match; its data is set when all lines are in place. */
static int
autocomplete_file_add (struct autocomplete_file *const file,
                       size_t const                    index,
                       struct cstr_view const          command,
                       struct cstr_view const          directory,
                       struct cstr_view const          name,
                       bool const                      is_directory,
                       struct cstr_view const          suffix)
{
	size_t const cursor = command.len + directory.len + name.len + 1;
	struct dstr *const lines = &file->lines;
	char const separator = is_directory ? '/' : ' ';
	if (!dstr_cat_n(lines, command.data, command.len) ||
	    !dstr_cat_n(lines, directory.data, directory.len) ||
	    !dstr_cat_n(lines, name.data, name.len) ||
	    !dstr_cat_n(lines, &separator, 1) ||
	    !dstr_cat_n(lines, suffix.data, suffix.len) ||
	    !dstr_cat_n(lines, "", 1))
		return ENOMEM;
	file->matches[index] = (struct autocomplete_match){
		.line = {nullptr, cursor + suffix.len},
		.cursor = cursor,
		.label_begin = (uint32_t)(command.len + directory.len),
		.kind = AUTOCOMPLETE_FILE,
	};
	return 0;
}

static bool
autocomplete_file_safe (struct cstr_view const name)
{
	return !(name.len == 1 && name.data[0] == '.') &&
	       !(name.len == 2 && name.data[0] == '.' && name.data[1] == '.') &&
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

	if (!file || !command.data || !command.len || cursor > line.len)
		return EINVAL;

	if (line.len > AUTOCOMPLETE_FILE_LIMIT)
		return E2BIG;

	if (!autocomplete_line(line))
		return EINVAL;

	if (cursor < command.len || !cstr_view_starts(line, command))
		return 0;

	struct cstr_view const prefix = { line.data + command.len, cursor - command.len };
	char const *const slash = memchr(line.data + cursor, '/', line.len - cursor);
	struct cstr_view const suffix = slash
		? (struct cstr_view){ slash + 1, line.len - (size_t)(slash + 1 - line.data) }
		: (struct cstr_view){};
	size_t directory_len = prefix.len;
	while (directory_len && prefix.data[directory_len - 1] != '/')
		--directory_len;
	struct cstr_view const directory = { prefix.data, directory_len };
	struct cstr_view const basename = { prefix.data + directory_len, prefix.len - directory_len };
	(void)dstr_clear(&file->lines);
	if (prefix.len == 1 && prefix.data[0] == '~') {
		int const e = autocomplete_file_add(file, 0, command, (struct cstr_view){},
		                                    prefix, true, suffix);
		if (!e) {
			file->matches[0].line.data = dstr_get(&file->lines);
			*matches = file->matches;
			*count = 1;
		}
		return e;
	}
	(void)dstr_clear(&file->directory);

	if (!directory_len) {
		if (!dstr_cat_n(&file->directory, ".", 1))
			return ENOMEM;

	} else if (directory_len >= 2 && prefix.data[0] == '~' &&
	           prefix.data[1] == '/' && dstr_len(&file->home)) {
		if (!dstr_cat_n(&file->directory, dstr_get(&file->home), dstr_len(&file->home)) ||
		    !dstr_cat_n(&file->directory, directory.data + 1, directory.len - 1))
			return ENOMEM;

	} else if (!dstr_cat_n(&file->directory, directory.data, directory.len)) {
		return ENOMEM;
	}

	DIR *dir = opendir(dstr_get(&file->directory));
	if (!dir)
		return errno == ENOMEM ? ENOMEM : 0;
	size_t used = 0;
	int e = 0;
	while (used < AUTOCOMPLETE_FILE_MATCHES) {
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
		size_t const length = command.len + directory.len + name.len + 1 + suffix.len;
		if (length >= AUTOCOMPLETE_FILE_LIMIT - dstr_len(&file->lines))
			break;
		e = autocomplete_file_add(file, used, command, directory, name, is_directory, suffix);
		if (e)
			break;
		++used;
	}

	if (closedir(dir) && !e)
		e = errno;
	dir = nullptr;
	if (e)
		return e;

	char const *data = dstr_get(&file->lines);
	for (size_t i = 0; i < used; ++i) {
		file->matches[i].line.data = data;
		data += file->matches[i].line.len + 1;
	}
	autocomplete_match_sort(file->matches, used, command.len + directory.len);
	*matches = file->matches;
	*count = used;
	return 0;
}
