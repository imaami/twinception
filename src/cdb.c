/** @file cdb.c
 *
 * The compile commands of a repository.
 */
#define _GNU_SOURCE /* memrchr() */
#include <errno.h>
#include <string.h>

#include "cdb.h"
#include "compat.h"
#include "cstr_view.h"
#include "dstr.h"
#include "jb.h"

/* The options whose value is a path, as the next word or joined. The value
 * of --sysroot=, the last one, can only be joined.
 */
static struct cstr_view const cdb_paths[] = {
	CSTR_VIEW_INIT("-I"), CSTR_VIEW_INIT("-iquote"), CSTR_VIEW_INIT("-isystem"),
	CSTR_VIEW_INIT("-idirafter"), CSTR_VIEW_INIT("-include"), CSTR_VIEW_INIT("-imacros"),
	CSTR_VIEW_INIT("-isysroot"), CSTR_VIEW_INIT("--sysroot="),
};

/* The options that are removed with the next word. Removed alone are -c,
 * the words that start with -M or -Xclang=, and the long forms of -M,
 * --dependencies to --write-user-dependencies.
 */
static struct cstr_view const cdb_pairs[] = {
	CSTR_VIEW_INIT("-o"), CSTR_VIEW_INIT("-MF"), CSTR_VIEW_INIT("-MT"),
	CSTR_VIEW_INIT("-MQ"), CSTR_VIEW_INIT("-MJ"), CSTR_VIEW_INIT("-Xclang"),
	CSTR_VIEW_INIT("-Xpreprocessor"), CSTR_VIEW_INIT("-mllvm"),
	CSTR_VIEW_INIT("-serialize-diagnostics"), CSTR_VIEW_INIT("--serialize-diagnostics"),
};

/* Appends the components of @p rest to @p out, where "." and empty ones
 * do nothing and ".." removes the last one.
 */
static bool
cdb_walk (struct dstr      *out,
          struct cstr_view  rest)
{
	bool ok = true;
	for (struct cstr_view part; ok && rest.len; ) {
		part = rest;
		if (!cstr_view_cut(&rest, '/', &part))
			rest.len = 0;
		if (!part.len || cstr_view_equal(part, CSTR_VIEW(".")))
			continue;
		if (!cstr_view_equal(part, CSTR_VIEW(".."))) {
			ok = dstr_cat_n(out, "/", 1) && dstr_cat_n(out, part.data, part.len);
			continue;
		}
		char const *slash = memrchr(dstr_get(out), '/', dstr_len(out));
		ok = !slash || dstr_erase(out, (size_t)(slash - dstr_get(out)), SIZE_MAX);
	}
	return ok;
}

int
cdb_norm (struct dstr      *out,
          struct cstr_view  dir,
          struct cstr_view  path)
{
	dstr_clear(out);
	bool const ok = (cstr_view_starts(path, CSTR_VIEW("/")) || cdb_walk(out, dir)) &&
	                cdb_walk(out, path);
	return ok && (dstr_len(out) || dstr_cat_n(out, "/", 1)) ? 0 : ENOMEM;
}

static bool
cdb_under (struct cstr_view path,
           struct cstr_view top)
{
	return cstr_view_starts(path, top) && (path.len == top.len || path.data[top.len] == '/');
}

/* Appends @p opt and the path @p value, which ends in a NUL, to @p words:
 * mapped into the tree if at or under the top, left out with @p opt if
 * above it, and else made absolute against @p dir.
 */
static int
cdb_path (struct cdb_map const *map,
          struct cstr_view      dir,
          struct cstr_view      opt,
          struct cstr_view      value,
          struct dstr          *tmp,
          struct dstr          *words)
{
	if (cdb_norm(tmp, dir, value))
		return ENOMEM;
	struct cstr_view const path = cstr_view_dstr(tmp);
	bool const under = cdb_under(path, map->top);
	if (!under && (path.len == 1 || cdb_under(map->top, path)))
		return 0;
	bool ok = dstr_cat_n(words, opt.data, opt.len);
	if (under)
		ok = ok && dstr_cat_n(words, map->tree.data, map->tree.len) &&
		     dstr_cat_n(words, path.data + map->top.len, path.len - map->top.len + 1);
	else if (value.len && *value.data == '/')
		ok = ok && dstr_cat_n(words, value.data, value.len + 1);
	else
		ok = ok && dstr_cat_n(words, dir.data, dir.len) && dstr_cat_n(words, "/", 1) &&
		     dstr_cat_n(words, value.data, value.len + 1);
	return ok ? 0 : ENOMEM;
}

/* Returns the "arguments" of @p entry if they are strings, or nullptr. */
static struct json_object *
cdb_arguments (struct json_object *entry)
{
	struct json_object *args = jb_get(entry, "arguments");
	size_t const n = jb_len(args);
	for (size_t i = 0; i < n; ++i)
		if (!jb_view(jb_at(args, i)).data)
			return nullptr;
	return n ? args : nullptr;
}

static int
cdb_words (struct cdb_map const *map,
           struct json_object   *args,
           struct cstr_view      dir,
           struct cstr_view      file,
           struct dstr          *tmp,
           struct dstr          *words)
{
	int e = dstr_cat_n(words, "clang", sizeof "clang") ? 0 : ENOMEM;
	for (size_t i = 1, n = jb_len(args); !e && i < n; ++i) {
		struct cstr_view const a = jb_view(jb_at(args, i));
		size_t o = 0;
		while (o < array_size(cdb_paths) && !cstr_view_starts(a, cdb_paths[o]))
			++o;
		size_t const at = o < array_size(cdb_paths) ? cdb_paths[o].len : a.len;
		struct cstr_view const value = cstr_view_slice(a, at, SIZE_MAX);
		/* -Wp, passes words to the preprocessor, which writes files for
		 * some of them, such as -MD. It stays only with one -D or -U. */
		struct cstr_view const wp = cstr_view_slice(a, 4, SIZE_MAX);
		bool const drop = cstr_view_equal(a, CSTR_VIEW("-c")) ||
		                  cstr_view_starts(a, CSTR_VIEW("-M")) ||
		                  cstr_view_starts(a, CSTR_VIEW("-Xclang=")) ||
		                  cstr_view_starts(a, CSTR_VIEW("@")) ||
		                  (cstr_view_starts(a, CSTR_VIEW("--")) &&
		                   cstr_view_ends(a, CSTR_VIEW("dependencies"))) ||
		                  (cstr_view_starts(a, CSTR_VIEW("-Wp,")) &&
		                   ((!cstr_view_starts(wp, CSTR_VIEW("-D")) &&
		                     !cstr_view_starts(wp, CSTR_VIEW("-U"))) ||
		                    memchr(wp.data, ',', wp.len)));
		if (cstr_view_index(cdb_pairs, array_size(cdb_pairs), a) < array_size(cdb_pairs))
			++i;
		else if (drop)
			continue;
		else if (o + 1 < array_size(cdb_paths) && !value.len && i + 1 < n)
			e = cdb_path(map, dir, (struct cstr_view){ a.data, a.len + 1 },
			             jb_view(jb_at(args, ++i)), tmp, words);
		/* A joined value that starts with '-' belongs to another option,
		 * such as -include-pch. */
		else if (value.len && *value.data != '-')
			e = cdb_path(map, dir, cstr_view_slice(a, 0, a.len - value.len), value, tmp,
			             words);
		else if (*a.data != '-' && ((e = cdb_norm(tmp, dir, a)) ||
		                            cstr_view_equal(cstr_view_dstr(tmp), file)))
			continue;
		else if (*a.data == '/')
			e = cdb_path(map, dir, (struct cstr_view){}, a, tmp, words);
		else if (!dstr_cat_n(words, a.data, a.len + 1))
			e = ENOMEM;
	}
	return e;
}

int
cdb_command (struct cdb_map const *map,
             struct json_object   *cmds,
             struct cstr_view      rel,
             struct dstr          *words)
{
	bool const header = cstr_view_ends(rel, CSTR_VIEW(".h"));
	struct dstr want = DSTR_INIT;
	struct dstr file = DSTR_INIT;
	struct dstr tmp = DSTR_INIT;
	int e = cdb_norm(&want, map->top, rel);
	struct cstr_view const w = cstr_view_dstr(&want);
	struct json_object *best = nullptr;
	size_t common = 0;
	bool usable = false;
	for (size_t i = 0; !e && i < jb_len(cmds); ++i) {
		struct json_object *entry = jb_at(cmds, i);
		struct cstr_view const dir = jb_view(jb_get(entry, "directory"));
		struct cstr_view const name = jb_view(jb_get(entry, "file"));
		if (!dir.data || !name.data || !cdb_arguments(entry) ||
		    (e = cdb_norm(&tmp, dir, name)) || !cdb_under(cstr_view_dstr(&tmp), map->top))
			continue;
		/* A header takes the entry with the longest common directory. */
		struct cstr_view const path = cstr_view_dstr(&tmp);
		size_t c = 0;
		for (size_t k = 0; k < path.len && k < w.len && path.data[k] == w.data[k]; ++k)
			c = path.data[k] == '/' ? k : c;
		usable = true;
		if (header ? !best || c > common : cstr_view_equal(path, w)) {
			best = entry;
			common = c;
		}
	}
	if (!e && !best)
		e = usable ? ENOENT : EINVAL;
	struct cstr_view const dir = jb_view(jb_get(best, "directory"));
	if (!e)
		e = cdb_norm(&file, dir, jb_view(jb_get(best, "file")));
	if (!e)
		e = cdb_words(map, cdb_arguments(best), dir, cstr_view_dstr(&file), &tmp, words);
	if (!e && header && !dstr_cat_n(words, "-x\0c-header", sizeof "-x\0c-header"))
		e = ENOMEM;
	if (!e)
		e = cdb_path(map, map->top, (struct cstr_view){}, w, &tmp, words);
	dstr_cleanup(&tmp);
	dstr_cleanup(&file);
	dstr_cleanup(&want);
	return e;
}
