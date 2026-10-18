/** @file
 * Filename completion with retained, byte-counted work buffers.
 */
#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "autocomplete_file.h"
#include "dstr.h"
#include "jb.h"

enum {
	AUTOCOMPLETE_FILE_MATCHES = 256,
	AUTOCOMPLETE_FILE_LIMIT = 16 << 20,
};

struct autocomplete_file {
	struct dstr               lines[AUTOCOMPLETE_FILE_MATCHES];
	struct autocomplete_match matches[AUTOCOMPLETE_FILE_MATCHES];
	struct dstr               directory;
	struct dstr               home;
};

static bool
autocomplete_file_utf8 (struct cstr_view const text,
                        bool const             controls)
{
	if (jb_utf8(text.data, text.len) < text.len)
		return false;

	for (size_t i = 0; i < text.len; ++i) {
		unsigned char const byte = (unsigned char)text.data[i];
		if (!byte || (!controls && (byte < 0x20 || byte == 0x7f ||
		    (byte == 0xc2 && (unsigned char)text.data[i + 1] < 0xa0))))
			return false;
	}

	return true;
}

struct autocomplete_file *
autocomplete_file_create (int *const e)
{
	struct autocomplete_file *file = malloc(sizeof *file);
	if (!file) {
		*e = ENOMEM;
		return nullptr;
	}
	for (size_t i = 0; i < AUTOCOMPLETE_FILE_MATCHES; ++i)
		file->lines[i] = dstr(nullptr);
	file->directory = dstr(nullptr);
	file->home = dstr(nullptr);
	char const *const value = getenv("HOME");
	struct cstr_view const home = cstr_view(value);
	if (autocomplete_file_utf8(home, true) && !dstr_cat_n(&file->home, home.data, home.len)) {
		autocomplete_file_destroy(&file);
		*e = ENOMEM;
		return nullptr;
	}
	*e = 0;
	return file;
}

void
autocomplete_file_destroy (struct autocomplete_file **const pp)
{
	if (!pp || !*pp)
		return;
	struct autocomplete_file *file = *pp;
	for (size_t i = 0; i < AUTOCOMPLETE_FILE_MATCHES; ++i)
		dstr_cleanup(&file->lines[i]);
	dstr_cleanup(&file->directory);
	dstr_cleanup(&file->home);
	free(file);
	file = nullptr;
	*pp = nullptr;
}

static int
autocomplete_file_add (struct autocomplete_file *const file,
                       size_t const                    index,
                       struct cstr_view const          command,
                       struct cstr_view const          directory,
                       struct cstr_view const          name,
                       bool const                      is_directory,
                       struct cstr_view const          suffix)
{
	size_t const cursor = command.len + directory.len + name.len + is_directory;
	if (cursor > AUTOCOMPLETE_FILE_LIMIT ||
	    suffix.len > AUTOCOMPLETE_FILE_LIMIT - cursor)
		return E2BIG;
	struct dstr *const line = &file->lines[index];
	(void)dstr_clear(line);
	if (!dstr_reserve(line, cursor + suffix.len) ||
	    !dstr_cat_n(line, command.data, command.len) ||
	    !dstr_cat_n(line, directory.data, directory.len) ||
	    !dstr_cat_n(line, name.data, name.len) ||
	    (is_directory && !dstr_cat_n(line, "/", 1)) ||
	    !dstr_cat_n(line, suffix.data, suffix.len))
		return ENOMEM;
	file->matches[index] = (struct autocomplete_match){
		.line = cstr_view_dstr(line),
		.cursor = cursor,
	};
	return 0;
}

static int
autocomplete_file_compare (struct autocomplete_match const *const x,
                           struct autocomplete_match const *const y,
                           size_t const                           prefix)
{
	return cstr_view_compare(cstr_view_slice(x->line, prefix, x->cursor - prefix),
	                         cstr_view_slice(y->line, prefix, y->cursor - prefix));
}

static void
autocomplete_file_heap (struct autocomplete_file *const file,
                        size_t                          root,
                        size_t const                    count,
                        size_t const                    prefix)
{
	while (root < count / 2) {
		size_t child = root * 2 + 1;
		if (child + 1 < count && autocomplete_file_compare(&file->matches[child], &file->matches[child + 1], prefix) < 0)
			++child;
		if (autocomplete_file_compare(&file->matches[root], &file->matches[child], prefix) >= 0)
			return;
		struct autocomplete_match const swap = file->matches[root];
		file->matches[root] = file->matches[child];
		file->matches[child] = swap;
		root = child;
	}
}

static void
autocomplete_file_sort (struct autocomplete_file *const file,
                        size_t                          count,
                        size_t const                    prefix)
{
	for (size_t i = count / 2; i;)
		autocomplete_file_heap(file, --i, count, prefix);
	while (count > 1) {
		struct autocomplete_match const swap = file->matches[0];
		file->matches[0] = file->matches[--count];
		file->matches[count] = swap;
		autocomplete_file_heap(file, 0, count, prefix);
	}
}

static bool
autocomplete_file_safe (struct cstr_view const name)
{
	return !(name.len == 1 && name.data[0] == '.') &&
	       !(name.len == 2 && name.data[0] == '.' && name.data[1] == '.') &&
	       autocomplete_file_utf8(name, false);
}

int
autocomplete_file_complete (struct autocomplete_file *const         file,
                            struct cstr_view const                  line,
                            size_t const                            cursor,
                            struct cstr_view const *const           cmds,
                            size_t const                            cmd_count,
                            struct autocomplete_match const **const matches,
                            size_t *const                           count)
{
	if (!matches || !count)
		return EINVAL;

	*matches = nullptr;
	*count = 0;

	if (!file || (!line.data && line.len) || (!cmds && cmd_count) || cursor > line.len)
		return EINVAL;

	if (line.len > AUTOCOMPLETE_FILE_LIMIT)
		return E2BIG;

	if (!autocomplete_file_utf8(line, true))
		return EINVAL;

	for (size_t c = 0; c < cmd_count; ++c) {
		struct cstr_view const command = cmds[c];
		if (!command.data || !command.len || cursor < command.len ||
		    !cstr_view_starts(line, command))
			continue;
		struct cstr_view const prefix = { line.data + command.len, cursor - command.len };
		char const *const slash = memchr(line.data + cursor, '/', line.len - cursor);
		struct cstr_view const suffix = slash
			? (struct cstr_view){ slash + 1, line.len - (size_t)(slash + 1 - line.data) }
			: (struct cstr_view){};
		if (prefix.len == 1 && prefix.data[0] == '~') {
			int const e = autocomplete_file_add(file, 0, command, (struct cstr_view){},
			                                    prefix, true, suffix);
			if (!e) {
				*matches = file->matches;
				*count = 1;
			}
			return e;
		}
		size_t directory_len = prefix.len;
		while (directory_len && prefix.data[directory_len - 1] != '/')
			--directory_len;
		struct cstr_view const directory = { prefix.data, directory_len };
		struct cstr_view const basename = { prefix.data + directory_len, prefix.len - directory_len };
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
		size_t bytes = 0;
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
			size_t const length = command.len + directory.len + name.len + is_directory + suffix.len;
			if (length >= AUTOCOMPLETE_FILE_LIMIT - bytes)
				break;
			e = autocomplete_file_add(file, used, command, directory, name, is_directory, suffix);
			if (e)
				break;
			bytes += length + 1;
			++used;
		}

		if (closedir(dir) && !e)
			e = errno;
		dir = nullptr;
		if (e)
			return e;

		autocomplete_file_sort(file, used, command.len + directory.len);
		*matches = file->matches;
		*count = used;
		return 0;
	}
	return 0;
}
