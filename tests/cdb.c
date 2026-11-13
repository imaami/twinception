/** @file Tests of the compile commands of a repository, src/cdb.c: the
 *  choice of the entry of a file, the words that are removed, and the
 *  paths that are mapped into the tree or removed.
 */
#include <errno.h>
#include <json-c/json.h>
#include <string.h>

#include "cdb.h"
#include "check.h"
#include "cstr_view.h"
#include "dstr.h"

/* Compile commands of a repository at /r/proj, built in /r/proj/build. */
static char const commands[] =
	"[{\"directory\": \"/r/proj/build\", \"file\": \"/r/proj//src/./a.c\", \"arguments\": ["
	"\"gcc\", \"-I/r/proj\", \"-I/r/proj/src\", \"-I/r/proj/../x\", \"-I/r/projx\","
	"\"-I/home/u\", \"-I\", \"/r/proj/inc\", \"-iquote\", \"../src\","
	"\"-isystem/r/proj/sys\", \"--sysroot=/r/proj/sr\", \"-include\", \"cfg.h\","
	"\"-o\", \"x.o\", \"-MF\", \"x.d\", \"-MT\", \"x\", \"-MQ\", \"x\", \"-Xclang\", \"-load\","
	"\"-mllvm\", \"-x\", \"-c\", \"-MMD\", \"-MP\", \"@rsp\", \"-target\", \"arm-none-eabi\","
	"\"-isysroot\", \"/opt/sdk\", \"-DX=1\", \"-include-pch\", \"p.pch\","
	"\"-I/r\", \"-I\", \"/\", \"-isystem\", \"/r/proj/..\", \"-MJ\", \"x.json\", \"-MJy.json\","
	"\"-MFz.d\", \"-MG\", \"-Wp,-MD,x.d\", \"-Wp,-MMD,x.d\", \"-Wp,-D_FORTIFY_SOURCE=2\","
	"\"-Wp,-dependency-file,x\", \"-Xpreprocessor\", \"-MD\", \"--write-dependencies\","
	"\"--write-user-dependencies\", \"-Xclang=-load\", \"-serialize-diagnostics\", \"x.dia\","
	"\"--serialize-diagnostics\", \"y.dia\","
	"\"/r/proj//src/./a.c\"]},"
	" {\"directory\": \"/r/proj/build\", \"file\": \"../tests/t.c\","
	"  \"arguments\": [\"cc\", \"-DT\", \"../tests/t.c\"]},"
	" {\"directory\": \"/r/proj/build\", \"file\": \"/r/proj/src/c.c\", \"command\": \"cc c.c\"},"
	" {\"directory\": \"/r\", \"file\": \"/r/o.c\", \"arguments\": [\"cc\", \"/r/o.c\"]}]";

/* The words for a.c with the tree /c/proj/tree, without the file. */
static char const a_words[] =
	"clang\0-I/c/proj/tree\0-I/c/proj/tree/src\0-I/r/proj/../x\0-I/r/projx\0"
	"-I/home/u\0-I\0/c/proj/tree/inc\0-iquote\0/c/proj/tree/src\0"
	"-isystem/c/proj/tree/sys\0--sysroot=/c/proj/tree/sr\0-include\0"
	"/c/proj/tree/build/cfg.h\0-target\0arm-none-eabi\0-isysroot\0/opt/sdk\0-DX=1\0"
	"-include-pch\0p.pch\0-Wp,-D_FORTIFY_SOURCE=2\0";

/* Returns the error of cdb_command() for @p rel in @p cmds. On success,
 * checks that the words are the @p len bytes at @p want, each word
 * followed by a NUL.
 */
static int
check_command (struct json_object *cmds,
               char const         *rel,
               char const         *want,
               size_t              len)
{
	struct cdb_map const map = { CSTR_VIEW("/r/proj"), CSTR_VIEW("/c/proj/tree") };
	struct dstr words = DSTR_INIT;
	int e = cdb_command(&map, cmds, cstr_view(rel), &words);
	if (!e)
		CHECK(cstr_view_equal(cstr_view_dstr(&words), cstr_view_n(want, len)));
	dstr_cleanup(&words);
	return e;
}

int
main (void)
{
	struct json_object *cmds = json_tokener_parse(commands);
	CHECK(cmds);

	/* For a .c file, the path of the file in the tree is the last word. */
	char const a_c[] = "/c/proj/tree/src/a.c";
	char want[sizeof a_words + sizeof "-x\0c-header" + sizeof a_c + 2];
	memcpy(want, a_words, sizeof a_words - 1);
	memcpy(want + sizeof a_words - 1, a_c, sizeof a_c);
	CHECK(!check_command(cmds, "src/a.c", want, sizeof a_words - 1 + sizeof a_c));

	/* A header uses the command of the .c file nearest to it. */
	char const a_h[] = "-x\0c-header\0/c/proj/tree/src/a.h";
	memcpy(want + sizeof a_words - 1, a_h, sizeof a_h);
	CHECK(!check_command(cmds, "src/a.h", want, sizeof a_words - 1 + sizeof a_h));

	/* A relative file is relative to the directory of its entry. */
	static char const t_c[] = "clang\0-DT\0/c/proj/tree/tests/t.c";
	CHECK(!check_command(cmds, "tests/t.c", t_c, sizeof t_c));
	static char const t_h[] = "clang\0-DT\0-x\0c-header\0/c/proj/tree/tests/t.h";
	CHECK(!check_command(cmds, "tests/t.h", t_h, sizeof t_h));

	/* ENOENT for a .c file without an entry or with an entry that
	 * contains only "command". */
	CHECK(check_command(cmds, "src/z.c", "", 0) == ENOENT);
	CHECK(check_command(cmds, "src/c.c", "", 0) == ENOENT);

	/* EINVAL if no entry with "arguments" is for a file under the top. */
	CHECK(!json_object_array_del_idx(cmds, 0, 2));
	CHECK(check_command(cmds, "src/c.c", "", 0) == EINVAL);
	CHECK(check_command(cmds, "src/c.h", "", 0) == EINVAL);
	json_object_put(cmds);
	return check_report("cdb");
}
