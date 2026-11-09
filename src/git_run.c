/** @file git_run.c
 *
 * The argv and the environment of git.
 */
#include <errno.h>
#include <string.h>

#include "buf.h"
#include "child.h"
#include "cstr_view.h"
#include "git_run.h"
#include "oid.h"
#include "util.h"

int
git_run_dir_init (struct git_run_dir *dest,
                  char const         *path,
                  char const *const  *opts,
                  char *const        *env,
                  uint32_t            flags)
{
	if (!dest || flags & ~(uint32_t)GIT_RUN_DOT)
		return EINVAL;
	*dest = (struct git_run_dir){
		.path  = path,
		.opts  = opts,
		.env   = { util_env_path(), GIT_ENV_FIXED },
		.flags = flags,
	};
	for (size_t k = 1 + GIT_RUN_ENV_FIXED; env && *env; ++env, ++k) {
		if (k + 1 == array_size(dest->env)) {
			*dest = (struct git_run_dir){};
			return E2BIG;
		}
		dest->env[k] = *env;
	}
	return 0;
}

int
git_run_dir_argv (struct git_run_dir const *dir,
                  char const *const        *words,
                  char const              **argv,
                  size_t                    cap)
{
	static char const *const fixed[] = { "git", GIT_OPTIONS_FIXED, nullptr };
	static char const *const dot[] = { "--git-dir=.", nullptr };
	if (!dir || !argv)
		return EINVAL;
	char const *const at[] = { "-C", dir->path, nullptr };
	char const *const *const parts[] = {
		fixed, dir->opts, dir->path ? at : nullptr, dir->flags & GIT_RUN_DOT ? dot : nullptr,
		words,
	};
	return child_argv(argv, cap, parts, array_size(parts));
}

int
git_run_init (struct git_run           *dest,
              struct git_run_dir const *dir,
              char const *const        *words,
              struct child_cmd const   *how)
{
	if (!dest || !words || !how)
		return EINVAL;
	int e = git_run_dir_argv(dir, words, dest->argv, array_size(dest->argv));
	if (e)
		return e;
	dest->c = *how;
	dest->c.file = "git";
	dest->c.argv = dest->argv;
	dest->c.envp = dir->env;
	return 0;
}

int
git_run_capture (struct git_run const *run,
                 struct child_fds      fds,
                 struct buf           *out,
                 struct oid           *oid)
{
	if (!run || (oid && !out))
		return EINVAL;
	struct child_end end;
	int e;
	if (out) {
		buf_clear(out);
		e = child_capture(&run->c, fds, out, &end);
	} else {
		e = child_run(&run->c, fds, &end);
	}
	if (!oid)
		return e ? e : end.status ? EIO : 0;
	*oid = (struct oid){};
	if (e || end.status == 1)
		return e;
	struct span const got = buf_peek(out);
	struct cstr_view const text = { (char const *)got.ptr, got.len };
	return end.status || oid_init_line(oid, text) ? EIO : 0;
}

int
git_run_path (struct cstr_view *rest,
              struct cstr_view *path)
{
	if (!rest || !path)
		return EINVAL;
	return cstr_view_cut(rest, '\n', path) && cstr_view_starts(*path, CSTR_VIEW("/")) &&
	       !memchr(path->data, '\0', path->len) ? 0 : EINVAL;
}
